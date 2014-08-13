//
// Copyright 2014 Ettus Research LLC
//

#include "ad9361_filter_taps.h"
#include "ad9361_gain_tables.h"
#include "ad9361_synth_lut.h"
#include "ad9361_client.h"
#include "ad9361_device.h"
#include <cstring>
#include <cmath>
#include <uhd/exception.hpp>
#include <uhd/utils/log.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/thread/thread.hpp>
#include <boost/scoped_array.hpp>
#include <boost/format.hpp>

////////////////////////////////////////////////////////////
// the following macros evaluate to a compile time constant
// macros By Tom Torfs - donated to the public domain

/* turn a numeric literal into a hex constant
(avoids problems with leading zeroes)
8-bit constants max value 0x11111111, always fits in unsigned long
*/
#define HEX__(n) 0x##n##LU

/* 8-bit conversion function */
#define B8__(x) ((x&0x0000000FLU)?1:0) \
+((x&0x000000F0LU)?2:0) \
+((x&0x00000F00LU)?4:0) \
+((x&0x0000F000LU)?8:0) \
+((x&0x000F0000LU)?16:0) \
+((x&0x00F00000LU)?32:0) \
+((x&0x0F000000LU)?64:0) \
+((x&0xF0000000LU)?128:0)

/* for upto 8-bit binary constants */
#define B8(d) ((unsigned char)B8__(HEX__(d)))
////////////////////////////////////////////////////////////


namespace uhd { namespace usrp {

/* This is a simple comparison for very large double-precision floating
 * point numbers. It is used to prevent re-tunes for frequencies that are
 * the same but not 'exactly' because of data precision issues. */
// TODO: see if we can avoid the need for this function
int freq_is_nearly_equal(double a, double b) {
    return std::max(a,b) - std::min(a,b) < 1;
}

/***********************************************************************
 * Filter functions
 **********************************************************************/

/* This function takes in the calculated maximum number of FIR taps, and
 * returns a number of taps that makes AD9361 happy. */
int get_num_taps(int max_num_taps) {

    int num_taps = 0;
    int num_taps_list[] = {16, 32, 48, 64, 80, 96, 112, 128};
    int i;
    for(i = 1; i < 8; i++) {
        if(max_num_taps >= num_taps_list[i]) {
            continue;
        } else {
            num_taps = num_taps_list[i - 1];
            break;
        }
    } if(num_taps == 0) { num_taps = 128; }

    return num_taps;
}

/* Program either the RX or TX FIR filter.
 *
 * The process is the same for both filters, but the function must be told
 * how many taps are in the filter, and given a vector of the taps
 * themselves.  */

void ad9361_device_t::_program_fir_filter(direction_t direction, int num_taps, boost::uint16_t *coeffs)
{
    boost::uint16_t base;

    /* RX and TX filters use largely identical sets of programming registers.
     Select the appropriate bank of registers here. */
    if (direction == RX) {
        base = 0x0f0;
    } else {
        base = 0x060;
    }

    /* Encode number of filter taps for programming register */
    boost::uint8_t reg_numtaps = (((num_taps / 16) - 1) & 0x07) << 5;

    /* Turn on the filter clock. */
    _io_iface->poke8(base + 5, reg_numtaps | 0x1a);
    boost::this_thread::sleep(boost::posix_time::milliseconds(1));

    /* Zero the unused taps just in case they have stale data */
    int addr;
    for (addr = num_taps; addr < 128; addr++) {
        _io_iface->poke8(base + 0, addr);
        _io_iface->poke8(base + 1, 0x0);
        _io_iface->poke8(base + 2, 0x0);
        _io_iface->poke8(base + 5, reg_numtaps | 0x1e);
        _io_iface->poke8(base + 4, 0x00);
        _io_iface->poke8(base + 4, 0x00);
    }

    /* Iterate through indirect programming of filter coeffs using ADI recomended procedure */
    for (addr = 0; addr < num_taps; addr++) {
        _io_iface->poke8(base + 0, addr);
        _io_iface->poke8(base + 1, (coeffs[addr]) & 0xff);
        _io_iface->poke8(base + 2, (coeffs[addr] >> 8) & 0xff);
        _io_iface->poke8(base + 5, reg_numtaps | 0x1e);
        _io_iface->poke8(base + 4, 0x00);
        _io_iface->poke8(base + 4, 0x00);
    }

    /* UG-671 states (page 25) (paraphrased and clarified):
     " After the table has been programmed, write to register BASE+5 with the write bit D2 cleared and D1 high.
     Then, write to register BASE+5 again with D1 clear, thus ensuring that the write bit resets internally
     before the clock stops. Wait 4 sample clock periods after setting D2 high while that data writes into the table"
     */

    _io_iface->poke8(base + 5, reg_numtaps | 0x1A);
    if (direction == RX) {
        _io_iface->poke8(base + 5, reg_numtaps | 0x18);
        _io_iface->poke8(base + 6, 0x02); /* Also turn on -6dB Rx gain here, to stop filter overfow.*/
    } else {
        _io_iface->poke8(base + 5, reg_numtaps | 0x19); /* Also turn on -6dB Tx gain here, to stop filter overfow.*/
    }
}


/* Program the RX FIR Filter. */
void ad9361_device_t::_setup_rx_fir(size_t num_taps)
{
    boost::scoped_array<boost::uint16_t> coeffs(new boost::uint16_t[num_taps]);
    for (size_t i = 0; i < num_taps; i++) {
        switch (num_taps) {
        case 128:
            coeffs[i] = boost::uint16_t(hb127_coeffs[i]);
            break;
        case 96:
            coeffs[i] = boost::uint16_t(hb95_coeffs[i]);
            break;
        case 64:
            coeffs[i] = boost::uint16_t(hb63_coeffs[i]);
            break;
        case 48:
            coeffs[i] = boost::uint16_t(hb47_coeffs[i]);
            break;
        default:
            throw uhd::runtime_error("[ad9361_device_t] Unsupported number of Rx FIR taps.");
        }
    }

    _program_fir_filter(RX, num_taps, coeffs.get());
}

/* Program the TX FIR Filter. */
void ad9361_device_t::_setup_tx_fir(size_t num_taps)
{
    boost::scoped_array<boost::uint16_t> coeffs(new boost::uint16_t[num_taps]);
    for (size_t i = 0; i < num_taps; i++) {
        switch (num_taps) {
        case 128:
            coeffs[i] = boost::uint16_t(hb127_coeffs[i]);
            break;
        case 96:
            coeffs[i] = boost::uint16_t(hb95_coeffs[i]);
            break;
        case 64:
            coeffs[i] = boost::uint16_t(hb63_coeffs[i]);
            break;
        case 48:
            coeffs[i] = boost::uint16_t(hb47_coeffs[i]);
            break;
        default:
            throw uhd::runtime_error("[ad9361_device_t] Unsupported number of Tx FIR taps.");
        }
    }

    _program_fir_filter(TX, num_taps, coeffs.get());
}

/***********************************************************************
 * Calibration functions
 ***********************************************************************/

/* Calibrate and lock the BBPLL.
 *
 * This function should be called anytime the BBPLL is tuned. */
void ad9361_device_t::_calibrate_lock_bbpll()
{
    _io_iface->poke8(0x03F, 0x05); // Start the BBPLL calibration
    _io_iface->poke8(0x03F, 0x01); // Clear the 'start' bit

    /* Increase BBPLL KV and phase margin. */
    _io_iface->poke8(0x04c, 0x86);
    _io_iface->poke8(0x04d, 0x01);
    _io_iface->poke8(0x04d, 0x05);

    /* Wait for BBPLL lock. */
    size_t count = 0;
    while (!(_io_iface->peek8(0x05e) & 0x80)) {
        if (count > 1000) {
            throw uhd::runtime_error("[ad9361_device_t] BBPLL not locked");
            break;
        }
        count++;
        boost::this_thread::sleep(boost::posix_time::milliseconds(2));
    }
}

/* Calibrate the synthesizer charge pumps.
 *
 * Technically, this calibration only needs to be done once, at device
 * initialization. */
void ad9361_device_t::_calibrate_synth_charge_pumps()
{
    /* If this function ever gets called, and the ENSM isn't already in the
     * ALERT state, then something has gone horribly wrong. */
    if ((_io_iface->peek8(0x017) & 0x0F) != 5) {
        throw uhd::runtime_error("[ad9361_device_t] AD9361 not in ALERT during cal");
    }

    /* Calibrate the RX synthesizer charge pump. */
    size_t count = 0;
    _io_iface->poke8(0x23d, 0x04);
    while (!(_io_iface->peek8(0x244) & 0x80)) {
        if (count > 5) {
            throw uhd::runtime_error("[ad9361_device_t] RX charge pump cal failure");
            break;
        }
        count++;
        boost::this_thread::sleep(boost::posix_time::milliseconds(1));
    }
    _io_iface->poke8(0x23d, 0x00);

    /* Calibrate the TX synthesizer charge pump. */
    count = 0;
    _io_iface->poke8(0x27d, 0x04);
    while (!(_io_iface->peek8(0x284) & 0x80)) {
        if (count > 5) {
            throw uhd::runtime_error("[ad9361_device_t] TX charge pump cal failure");
            break;
        }
        count++;
        boost::this_thread::sleep(boost::posix_time::milliseconds(1));
    }
    _io_iface->poke8(0x27d, 0x00);
}

/* Calibrate the analog BB RX filter.
 *
 * Note that the filter calibration depends heavily on the baseband
 * bandwidth, so this must be re-done after any change to the RX sample
 * rate. */
double ad9361_device_t::_calibrate_baseband_rx_analog_filter()
{
    /* For filter tuning, baseband BW is half the complex BW, and must be
     * between 28e6 and 0.2e6. */
    double bbbw = _baseband_bw / 2.0;
    if (bbbw > 28e6) {
        bbbw = 28e6;
    } else if (bbbw < 0.20e6) {
        bbbw = 0.20e6;
    }

    double rxtune_clk = ((1.4 * bbbw * 2 * M_PI) / M_LN2);
    _rx_bbf_tunediv = std::min<boost::uint16_t>(511, std::ceil<boost::uint16_t>(_bbpll_freq / rxtune_clk));
    _regs.bbftune_config = (_regs.bbftune_config & 0xFE)
            | ((_rx_bbf_tunediv >> 8) & 0x0001);

    double bbbw_mhz = bbbw / 1e6;
    double temp = ((bbbw_mhz - std::floor(bbbw_mhz)) * 1000) / 7.8125;
    boost::uint8_t bbbw_khz = std::min<boost::uint8_t>(127, (std::floor<boost::uint8_t>(temp + 0.5)));

    /* Set corner frequencies and dividers. */
    _io_iface->poke8(0x1fb, (boost::uint8_t) (bbbw_mhz));
    _io_iface->poke8(0x1fc, bbbw_khz);
    _io_iface->poke8(0x1f8, (_rx_bbf_tunediv & 0x00FF));
    _io_iface->poke8(0x1f9, _regs.bbftune_config);

    /* RX Mix Voltage settings - only change with apps engineer help. */
    _io_iface->poke8(0x1d5, 0x3f);
    _io_iface->poke8(0x1c0, 0x03);

    /* Enable RX1 & RX2 filter tuners. */
    _io_iface->poke8(0x1e2, 0x02);
    _io_iface->poke8(0x1e3, 0x02);

    /* Run the calibration! */
    size_t count = 0;
    _io_iface->poke8(0x016, 0x80);
    while (_io_iface->peek8(0x016) & 0x80) {
        if (count > 100) {
            throw uhd::runtime_error("[ad9361_device_t] RX baseband filter cal FAILURE");
            break;
        }
        count++;
        boost::this_thread::sleep(boost::posix_time::milliseconds(1));
    }

    /* Disable RX1 & RX2 filter tuners. */
    _io_iface->poke8(0x1e2, 0x03);
    _io_iface->poke8(0x1e3, 0x03);

    return bbbw;
}

/* Calibrate the analog BB TX filter.
 *
 * Note that the filter calibration depends heavily on the baseband
 * bandwidth, so this must be re-done after any change to the TX sample
 * rate. */
double ad9361_device_t::_calibrate_baseband_tx_analog_filter()
{
    /* For filter tuning, baseband BW is half the complex BW, and must be
     * between 28e6 and 0.2e6. */
    double bbbw = _baseband_bw / 2.0;
    if (bbbw > 20e6) {
        bbbw = 20e6;
    } else if (bbbw < 0.625e6) {
        bbbw = 0.625e6;
    }

    double txtune_clk = ((1.6 * bbbw * 2 * M_PI) / M_LN2);
    boost::uint16_t txbbfdiv = std::min<boost::uint16_t>(511, (std::ceil<boost::uint16_t>(_bbpll_freq / txtune_clk)));
    _regs.bbftune_mode = (_regs.bbftune_mode & 0xFE)
            | ((txbbfdiv >> 8) & 0x0001);

    /* Program the divider values. */
    _io_iface->poke8(0x0d6, (txbbfdiv & 0x00FF));
    _io_iface->poke8(0x0d7, _regs.bbftune_mode);

    /* Enable the filter tuner. */
    _io_iface->poke8(0x0ca, 0x22);

    /* Calibrate! */
    size_t count = 0;
    _io_iface->poke8(0x016, 0x40);
    while (_io_iface->peek8(0x016) & 0x40) {
        if (count > 100) {
            throw uhd::runtime_error("[ad9361_device_t] TX baseband filter cal FAILURE");
            break;
        }

        count++;
        boost::this_thread::sleep(boost::posix_time::milliseconds(1));
    }

    /* Disable the filter tuner. */
    _io_iface->poke8(0x0ca, 0x26);

    return bbbw;
}

/* Calibrate the secondary TX filter.
 *
 * This filter also depends on the TX sample rate, so if a rate change is
 * made, the previous calibration will no longer be valid. */
void ad9361_device_t::_calibrate_secondary_tx_filter()
{
    /* For filter tuning, baseband BW is half the complex BW, and must be
     * between 20e6 and 0.53e6. */
    double bbbw = _baseband_bw / 2.0;
    if (bbbw > 20e6) {
        bbbw = 20e6;
    } else if (bbbw < 0.53e6) {
        bbbw = 0.53e6;
    }

    double bbbw_mhz = bbbw / 1e6;

    /* Start with a resistor value of 100 Ohms. */
    int res = 100;

    /* Calculate target corner frequency. */
    double corner_freq = 5 * bbbw_mhz * 2 * M_PI;

    /* Iterate through RC values to determine correct combination. */
    int cap = 0;
    int i;
    for (i = 0; i <= 3; i++) {
        cap = (std::floor<int>(0.5 + ((1 / ((corner_freq * res) * 1e6)) * 1e12)))
                - 12;

        if (cap <= 63) {
            break;
        }

        res = res * 2;
    }
    if (cap > 63) {
        cap = 63;
    }

    boost::uint8_t reg0d0, reg0d1, reg0d2;

    /* Translate baseband bandwidths to register settings. */
    if ((bbbw_mhz * 2) <= 9) {
        reg0d0 = 0x59;
    } else if (((bbbw_mhz * 2) > 9) && ((bbbw_mhz * 2) <= 24)) {
        reg0d0 = 0x56;
    } else if ((bbbw_mhz * 2) > 24) {
        reg0d0 = 0x57;
    } else {
        throw uhd::runtime_error("[ad9361_device_t] Cal2ndTxFil: INVALID_CODE_PATH bad bbbw_mhz");
        reg0d0 = 0x00;
    }

    /* Translate resistor values to register settings. */
    if (res == 100) {
        reg0d1 = 0x0c;
    } else if (res == 200) {
        reg0d1 = 0x04;
    } else if (res == 400) {
        reg0d1 = 0x03;
    } else if (res == 800) {
        reg0d1 = 0x01;
    } else {
        reg0d1 = 0x0c;
    }

    reg0d2 = cap;

    /* Program the above-calculated values. Sweet. */
    _io_iface->poke8(0x0d2, reg0d2);
    _io_iface->poke8(0x0d1, reg0d1);
    _io_iface->poke8(0x0d0, reg0d0);
}

/* Calibrate the RX TIAs.
 *
 * Note that the values in the TIA register, after calibration, vary with
 * the RX gain settings. */
void ad9361_device_t::_calibrate_rx_TIAs()
{
    boost::uint8_t reg1eb = _io_iface->peek8(0x1eb) & 0x3F;
    boost::uint8_t reg1ec = _io_iface->peek8(0x1ec) & 0x7F;
    boost::uint8_t reg1e6 = _io_iface->peek8(0x1e6) & 0x07;
    boost::uint8_t reg1db = 0x00;
    boost::uint8_t reg1dc = 0x00;
    boost::uint8_t reg1dd = 0x00;
    boost::uint8_t reg1de = 0x00;
    boost::uint8_t reg1df = 0x00;

    /* For calibration, baseband BW is half the complex BW, and must be
     * between 28e6 and 0.2e6. */
    double bbbw = _baseband_bw / 2.0;
    if (bbbw > 20e6) {
        bbbw = 20e6;
    } else if (bbbw < 0.20e6) {
        bbbw = 0.20e6;
    }
    double ceil_bbbw_mhz = std::ceil(bbbw / 1e6);

    /* Do some crazy resistor and capacitor math. */
    int Cbbf = (reg1eb * 160) + (reg1ec * 10) + 140;
    int R2346 = 18300 * (reg1e6 & 0x07);
    double CTIA_fF = (Cbbf * R2346 * 0.56) / 3500;

    /* Translate baseband BW to register settings. */
    if (ceil_bbbw_mhz <= 3) {
        reg1db = 0xe0;
    } else if ((ceil_bbbw_mhz > 3) && (ceil_bbbw_mhz <= 10)) {
        reg1db = 0x60;
    } else if (ceil_bbbw_mhz > 10) {
        reg1db = 0x20;
    } else {
        throw uhd::runtime_error("[ad9361_device_t] CalRxTias: INVALID_CODE_PATH bad bbbw_mhz");
    }

    if (CTIA_fF > 2920) {
        reg1dc = 0x40;
        reg1de = 0x40;
        boost::uint8_t temp = (boost::uint8_t) std::min<boost::uint8_t>(127,
                (std::floor<boost::uint8_t>(0.5 + ((CTIA_fF - 400.0) / 320.0))));
        reg1dd = temp;
        reg1df = temp;
    } else {
        boost::uint8_t temp = std::floor<boost::uint8_t>(0.5 + ((CTIA_fF - 400.0) / 40.0))
                + 0x40;
        reg1dc = temp;
        reg1de = temp;
        reg1dd = 0;
        reg1df = 0;
    }

    /* w00t. Settings calculated. Program them and roll out. */
    _io_iface->poke8(0x1db, reg1db);
    _io_iface->poke8(0x1dd, reg1dd);
    _io_iface->poke8(0x1df, reg1df);
    _io_iface->poke8(0x1dc, reg1dc);
    _io_iface->poke8(0x1de, reg1de);
}

/* Setup the AD9361 ADC.
 *
 * There are 40 registers that control the ADC's operation, most of the
 * values of which must be derived mathematically, dependent on the current
 * setting of the BBPLL. Note that the order of calculation is critical, as
 * some of the 40 registers depend on the values in others. */
void ad9361_device_t::_setup_adc()
{
    double bbbw_mhz = (((_bbpll_freq / 1e6) / _rx_bbf_tunediv) * M_LN2) \
                  / (1.4 * 2 * M_PI);

    /* For calibration, baseband BW is half the complex BW, and must be
     * between 28e6 and 0.2e6. */
    if(bbbw_mhz > 28) {
        bbbw_mhz = 28;
    } else if (bbbw_mhz < 0.20) {
        bbbw_mhz = 0.20;
    }

    boost::uint8_t rxbbf_c3_msb = _io_iface->peek8(0x1eb) & 0x3F;
    boost::uint8_t rxbbf_c3_lsb = _io_iface->peek8(0x1ec) & 0x7F;
    boost::uint8_t rxbbf_r2346 = _io_iface->peek8(0x1e6) & 0x07;

    double fsadc = _adcclock_freq / 1e6;

    /* Sort out the RC time constant for our baseband bandwidth... */
    double rc_timeconst = 0.0;
    if(bbbw_mhz < 18) {
        rc_timeconst = (1 / ((1.4 * 2 * M_PI) \
                            * (18300 * rxbbf_r2346)
                            * ((160e-15 * rxbbf_c3_msb)
                                + (10e-15 * rxbbf_c3_lsb) + 140e-15)
                            * (bbbw_mhz * 1e6)));
    } else {
        rc_timeconst = (1 / ((1.4 * 2 * M_PI) \
                            * (18300 * rxbbf_r2346)
                            * ((160e-15 * rxbbf_c3_msb)
                                + (10e-15 * rxbbf_c3_lsb) + 140e-15)
                            * (bbbw_mhz * 1e6) * (1 + (0.01 * (bbbw_mhz - 18)))));
    }

    double scale_res = sqrt(1 / rc_timeconst);
    double scale_cap = sqrt(1 / rc_timeconst);

    double scale_snr = (_adcclock_freq < 80e6) ? 1.0 : 1.584893192;
    double maxsnr = 640 / 160;

    /* Calculate the values for all 40 settings registers.
     *
     * DO NOT TOUCH THIS UNLESS YOU KNOW EXACTLY WHAT YOU ARE DOING. kthx.*/
    boost::uint8_t data[40];
    data[0] = 0;    data[1] = 0; data[2] = 0; data[3] = 0x24;
    data[4] = 0x24; data[5] = 0; data[6] = 0;
    data[7] = std::min<boost::uint8_t>(124, (std::floor<boost::uint8_t>(-0.5
                    + (80.0 * scale_snr * scale_res
                    * std::min<double>(1.0, sqrt(maxsnr * fsadc / 640.0))))));
    double data007 = data[7];
    data[8] = std::min<boost::uint8_t>(255, (std::floor<boost::uint8_t>(0.5
                    + ((20.0 * (640.0 / fsadc) * ((data007 / 80.0))
                    / (scale_res * scale_cap))))));
    data[10] = std::min<boost::uint8_t>(127, (std::floor<boost::uint8_t>(-0.5 + (77.0 * scale_res
                    * std::min<double>(1.0, sqrt(maxsnr * fsadc / 640.0))))));
    double data010 = data[10];
    data[9] = std::min<boost::uint8_t>(127, (std::floor<boost::uint8_t>(0.8 * data010)));
    data[11] = std::min<boost::uint8_t>(255, (std::floor<boost::uint8_t>(0.5
                    + (20.0 * (640.0 / fsadc) * ((data010 / 77.0)
                    / (scale_res * scale_cap))))));
    data[12] = std::min<boost::uint8_t>(127, (std::floor<boost::uint8_t>(-0.5
                    + (80.0 * scale_res * std::min<double>(1.0,
                    sqrt(maxsnr * fsadc / 640.0))))));
    double data012 = data[12];
    data[13] = std::min<boost::uint8_t>(255, (std::floor<boost::uint8_t>(-1.5
                    + (20.0 * (640.0 / fsadc) * ((data012 / 80.0)
                    / (scale_res * scale_cap))))));
    data[14] = 21 * (std::floor<boost::uint8_t>(0.1 * 640.0 / fsadc));
    data[15] = std::min<boost::uint8_t>(127, (1.025 * data007));
    double data015 = data[15];
    data[16] = std::min<boost::uint8_t>(127, (std::floor<boost::uint8_t>((data015
                    * (0.98 + (0.02 * std::max<double>(1.0,
                    (640.0 / fsadc) / maxsnr)))))));
    data[17] = data[15];
    data[18] = std::min<boost::uint8_t>(127, (0.975 * (data010)));
    double data018 = data[18];
    data[19] = std::min<boost::uint8_t>(127, (std::floor<boost::uint8_t>((data018
                    * (0.98 + (0.02 * std::max<double>(1.0,
                    (640.0 / fsadc) / maxsnr)))))));
    data[20] = data[18];
    data[21] = std::min<boost::uint8_t>(127, (0.975 * data012));
    double data021 = data[21];
    data[22] = std::min<boost::uint8_t>(127, (std::floor<boost::uint8_t>((data021
                    * (0.98 + (0.02 * std::max<double>(1.0,
                    (640.0 / fsadc) / maxsnr)))))));
    data[23] = data[21];
    data[24] = 0x2e;
    data[25] = std::floor<boost::uint8_t>(128.0 + std::min<double>(63.0,
                    63.0 * (fsadc / 640.0)));
    data[26] = std::floor<boost::uint8_t>(std::min<double>(63.0, 63.0 * (fsadc / 640.0)
                    * (0.92 + (0.08 * (640.0 / fsadc)))));
    data[27] = std::floor<boost::uint8_t>(std::min<double>(63.0,
                    32.0 * sqrt(fsadc / 640.0)));
    data[28] = std::floor<boost::uint8_t>(128.0 + std::min<double>(63.0,
                    63.0 * (fsadc / 640.0)));
    data[29] = std::floor<boost::uint8_t>(std::min<double>(63.0,
                    63.0 * (fsadc / 640.0)
                    * (0.92 + (0.08 * (640.0 / fsadc)))));
    data[30] = std::floor<boost::uint8_t>(std::min<double>(63.0,
                    32.0 * sqrt(fsadc / 640.0)));
    data[31] = std::floor<boost::uint8_t>(128.0 + std::min<double>(63.0,
                    63.0 * (fsadc / 640.0)));
    data[32] = std::floor<boost::uint8_t>(std::min<double>(63.0,
                    63.0 * (fsadc / 640.0) * (0.92
                    + (0.08 * (640.0 / fsadc)))));
    data[33] = std::floor<boost::uint8_t>(std::min<double>(63.0,
                    63.0 * sqrt(fsadc / 640.0)));
    data[34] = std::min<boost::uint8_t>(127, (std::floor<boost::uint8_t>(64.0
                    * sqrt(fsadc / 640.0))));
    data[35] = 0x40;
    data[36] = 0x40;
    data[37] = 0x2c;
    data[38] = 0x00;
    data[39] = 0x00;

    /* Program the registers! */
    for(size_t i = 0; i < 40; i++) {
        _io_iface->poke8(0x200+i, data[i]);
    }
}

/* Calibrate the baseband DC offset.
 *
 * Note that this function is called from within the TX quadrature
 * calibration function! */
void ad9361_device_t::_calibrate_baseband_dc_offset()
{
    _io_iface->poke8(0x193, 0x3f); // Calibration settings
    _io_iface->poke8(0x190, 0x0f); // Set tracking coefficient
    //write_ad9361_reg(device, 0x190, /*0x0f*//*0xDF*/0x80*1 | 0x40*1 | (16+8/*+4*/)); // Set tracking coefficient: don't *4 counter, do decim /4, increased gain shift
    _io_iface->poke8(0x194, 0x01); // More calibration settings

    /* Start that calibration, baby. */
    size_t count = 0;
    _io_iface->poke8(0x016, 0x01);
    while (_io_iface->peek8(0x016) & 0x01) {
        if (count > 100) {
            throw uhd::runtime_error("[ad9361_device_t] Baseband DC Offset Calibration Failure");
            break;
        }
        count++;
        boost::this_thread::sleep(boost::posix_time::milliseconds(5));
    }
}

/* Calibrate the RF DC offset.
 *
 * Note that this function is called from within the TX quadrature
 * calibration function. */
void ad9361_device_t::_calibrate_rf_dc_offset()
{
    /* Some settings are frequency-dependent. */
    if (_rx_freq < 4e9) {
        _io_iface->poke8(0x186, 0x32); // RF DC Offset count
        _io_iface->poke8(0x187, 0x24);
        _io_iface->poke8(0x188, 0x05);
    } else {
        _io_iface->poke8(0x186, 0x28); // RF DC Offset count
        _io_iface->poke8(0x187, 0x34);
        _io_iface->poke8(0x188, 0x06);
    }

    _io_iface->poke8(0x185, 0x20); // RF DC Offset wait count
    _io_iface->poke8(0x18b, 0x83);
    _io_iface->poke8(0x189, 0x30);

    /* Run the calibration! */
    size_t count = 0;
    _io_iface->poke8(0x016, 0x02);
    while (_io_iface->peek8(0x016) & 0x02) {
        if (count > 100) {
            throw uhd::runtime_error("[ad9361_device_t] RF DC Offset Calibration Failure");
            break;
        }
        count++;
        boost::this_thread::sleep(boost::posix_time::milliseconds(50));
    }
}

/* Start the RX quadrature calibration.
 *
 * Note that we are using AD9361's 'tracking' feature for RX quadrature
 * calibration, so once it starts it continues to free-run during operation.
 * It should be re-run for large frequency changes. */
void ad9361_device_t::_calibrate_rx_quadrature()
{
    /* Configure RX Quadrature calibration settings. */
    _io_iface->poke8(0x168, 0x03); // Set tone level for cal
    _io_iface->poke8(0x16e, 0x25); // RX Gain index to use for cal
    _io_iface->poke8(0x16a, 0x75); // Set Kexp phase
    _io_iface->poke8(0x16b, 0x15); // Set Kexp amplitude
    _io_iface->poke8(0x169, 0xcf); // Continuous tracking mode
    _io_iface->poke8(0x18b, 0xad);
}

/* TX quadtrature calibration routine.
 *
 * The TX quadrature needs to be done twice, once for each TX chain, with
 * only one register change in between. Thus, this function enacts the
 * calibrations, and it is called from calibrate_tx_quadrature. */
void ad9361_device_t::_tx_quadrature_cal_routine() {
    /* This is a weird process, but here is how it works:
     * 1) Read the calibrated NCO frequency bits out of 0A3.
     * 2) Write the two bits to the RX NCO freq part of 0A0.
     * 3) Re-read 0A3 to get bits [5:0] because maybe they changed?
     * 4) Update only the TX NCO freq bits in 0A3.
     * 5) Profit (I hope). */
    boost::uint8_t reg0a3 = _io_iface->peek8(0x0a3);
    boost::uint8_t nco_freq = (reg0a3 & 0xC0);
    _io_iface->poke8(0x0a0, 0x15 | (nco_freq >> 1));
    reg0a3 = _io_iface->peek8(0x0a3);
    _io_iface->poke8(0x0a3, (reg0a3 & 0x3F) | nco_freq);

    /* It is possible to reach a configuration that won't operate correctly,
     * where the two test tones used for quadrature calibration are outside
     * of the RX BBF, and therefore don't make it to the ADC. We will check
     * for that scenario here. */
    double max_cal_freq = (((_baseband_bw * _tfir_factor)
            * ((nco_freq >> 6) + 1)) / 32) * 2;
    double bbbw = _baseband_bw / 2.0; // bbbw represents the one-sided BW
    if (bbbw > 28e6) {
        bbbw = 28e6;
    } else if (bbbw < 0.20e6) {
        bbbw = 0.20e6;
    }
    if (max_cal_freq > bbbw)
        throw uhd::runtime_error("[ad9361_device_t] max_cal_freq > bbbw");

    _io_iface->poke8(0x0a1, 0x7B); // Set tracking coefficient
    _io_iface->poke8(0x0a9, 0xff); // Cal count
    _io_iface->poke8(0x0a2, 0x7f); // Cal Kexp
    _io_iface->poke8(0x0a5, 0x01); // Cal magnitude threshold VVVV
    _io_iface->poke8(0x0a6, 0x01);

    /* The gain table index used for calibration must be adjusted for the
     * mid-table to get a TIA index = 1 and LPF index = 0. */
    if ((_rx_freq >= 1300e6) && (_rx_freq < 4000e6)) {
        _io_iface->poke8(0x0aa, 0x22); // Cal gain table index
    } else {
        _io_iface->poke8(0x0aa, 0x25); // Cal gain table index
    }

    _io_iface->poke8(0x0a4, 0xf0); // Cal setting conut
    _io_iface->poke8(0x0ae, 0x00); // Cal LPF gain index (split mode)

    /* First, calibrate the baseband DC offset. */
    _calibrate_baseband_dc_offset();

    /* Second, calibrate the RF DC offset. */
    _calibrate_rf_dc_offset();

    /* Now, calibrate the TX quadrature! */
    size_t count = 0;
    _io_iface->poke8(0x016, 0x10);
    while (_io_iface->peek8(0x016) & 0x10) {
        if (count > 100) {
            throw uhd::runtime_error("[ad9361_device_t] TX Quadrature Calibration Failure");
            break;
        }
        count++;
        boost::this_thread::sleep(boost::posix_time::milliseconds(10));
    }
}

/* Run the TX quadrature calibration.
 *
 * Note that from within this function we are also triggering the baseband
 * and RF DC calibrations. */
void ad9361_device_t::_calibrate_tx_quadrature()
{
    /* Make sure we are, in fact, in the ALERT state. If not, something is
     * terribly wrong in the driver execution flow. */
    if ((_io_iface->peek8(0x017) & 0x0F) != 5) {
        throw uhd::runtime_error("[ad9361_device_t] TX Quad Cal started, but not in ALERT");
    }

    /* Turn off free-running and continuous calibrations. Note that this
     * will get turned back on at the end of the RX calibration routine. */
    _io_iface->poke8(0x169, 0xc0);

    /* This calibration must be done in a certain order, and for both TX_A
     * and TX_B, separately. Store the original setting so that we can
     * restore it later. */
    boost::uint8_t orig_reg_inputsel = _regs.inputsel;

    /***********************************************************************
     * TX1/2-A Calibration
     **********************************************************************/
    _regs.inputsel = _regs.inputsel & 0xBF;
    _io_iface->poke8(0x004, _regs.inputsel);

    _tx_quadrature_cal_routine();

    /***********************************************************************
     * TX1/2-B Calibration
     **********************************************************************/
    _regs.inputsel = _regs.inputsel | 0x40;
    _io_iface->poke8(0x004, _regs.inputsel);

    _tx_quadrature_cal_routine();

    /***********************************************************************
     * fin
     **********************************************************************/
    _regs.inputsel = orig_reg_inputsel;
    _io_iface->poke8(0x004, orig_reg_inputsel);
}


/***********************************************************************
 * Other Misc Setup Functions
 ***********************************************************************/

/* Program the mixer gain table.
 *
 * Note that this table is fixed for all frequency settings. */
void ad9361_device_t::_program_mixer_gm_subtable()
{
    boost::uint8_t gain[] = { 0x78, 0x74, 0x70, 0x6C, 0x68, 0x64, 0x60, 0x5C, 0x58,
            0x54, 0x50, 0x4C, 0x48, 0x30, 0x18, 0x00 };
    boost::uint8_t gm[] = { 0x00, 0x0D, 0x15, 0x1B, 0x21, 0x25, 0x29, 0x2C, 0x2F, 0x31,
            0x33, 0x34, 0x35, 0x3A, 0x3D, 0x3E };

    /* Start the clock. */
    _io_iface->poke8(0x13f, 0x02);

    /* Program the GM Sub-table. */
    int i;
    for (i = 15; i >= 0; i--) {
        _io_iface->poke8(0x138, i);
        _io_iface->poke8(0x139, gain[(15 - i)]);
        _io_iface->poke8(0x13A, 0x00);
        _io_iface->poke8(0x13B, gm[(15 - i)]);
        _io_iface->poke8(0x13F, 0x06);
        _io_iface->poke8(0x13C, 0x00);
        _io_iface->poke8(0x13C, 0x00);
    }

    /* Clear write bit and stop clock. */
    _io_iface->poke8(0x13f, 0x02);
    _io_iface->poke8(0x13C, 0x00);
    _io_iface->poke8(0x13C, 0x00);
    _io_iface->poke8(0x13f, 0x00);
}

/* Program the gain table.
 *
 * There are three different gain tables for different frequency ranges! */
void ad9361_device_t::_program_gain_table() {
    /* Figure out which gain table we should be using for our current
     * frequency band. */
    boost::uint8_t (*gain_table)[5] = NULL;
    boost::uint8_t new_gain_table;
    if (_rx_freq < 1300e6) {
        gain_table = gain_table_sub_1300mhz;
        new_gain_table = 1;
    } else if (_rx_freq < 4e9) {
        gain_table = gain_table_1300mhz_to_4000mhz;
        new_gain_table = 2;
    } else if (_rx_freq <= 6e9) {
        gain_table = gain_table_4000mhz_to_6000mhz;
        new_gain_table = 3;
    } else {
        throw uhd::runtime_error("[ad9361_device_t] Wrong _rx_freq value");
        new_gain_table = 1;
    }

    /* Only re-program the gain table if there has been a band change. */
    if (_curr_gain_table == new_gain_table) {
        return;
    } else {
        _curr_gain_table = new_gain_table;
    }

    /* Okay, we have to program a new gain table. Sucks, brah. Start the
     * gain table clock. */
    _io_iface->poke8(0x137, 0x1A);

    /* IT'S PROGRAMMING TIME. */
    boost::uint8_t index = 0;
    for (; index < 77; index++) {
        _io_iface->poke8(0x130, index);
        _io_iface->poke8(0x131, gain_table[index][1]);
        _io_iface->poke8(0x132, gain_table[index][2]);
        _io_iface->poke8(0x133, gain_table[index][3]);
        _io_iface->poke8(0x137, 0x1E);
        _io_iface->poke8(0x134, 0x00);
        _io_iface->poke8(0x134, 0x00);
    }

    /* Everything above the 77th index is zero. */
    for (; index < 91; index++) {
        _io_iface->poke8(0x130, index);
        _io_iface->poke8(0x131, 0x00);
        _io_iface->poke8(0x132, 0x00);
        _io_iface->poke8(0x133, 0x00);
        _io_iface->poke8(0x137, 0x1E);
        _io_iface->poke8(0x134, 0x00);
        _io_iface->poke8(0x134, 0x00);
    }

    /* Clear the write bit and stop the gain clock. */
    _io_iface->poke8(0x137, 0x1A);
    _io_iface->poke8(0x134, 0x00);
    _io_iface->poke8(0x134, 0x00);
    _io_iface->poke8(0x137, 0x00);
}

/* Setup gain control registers.
 *
 * This really only needs to be done once, at initialization. */
void ad9361_device_t::_setup_gain_control()
{
    _io_iface->poke8(0x0FA, 0xE0); // Gain Control Mode Select
    _io_iface->poke8(0x0FB, 0x08); // Table, Digital Gain, Man Gain Ctrl
    _io_iface->poke8(0x0FC, 0x23); // Incr Step Size, ADC Overrange Size
    _io_iface->poke8(0x0FD, 0x4C); // Max Full/LMT Gain Table Index
    _io_iface->poke8(0x0FE, 0x44); // Decr Step Size, Peak Overload Time
    _io_iface->poke8(0x100, 0x6F); // Max Digital Gain
    _io_iface->poke8(0x104, 0x2F); // ADC Small Overload Threshold
    _io_iface->poke8(0x105, 0x3A); // ADC Large Overload Threshold
    _io_iface->poke8(0x107, 0x31); // Large LMT Overload Threshold
    _io_iface->poke8(0x108, 0x39); // Small LMT Overload Threshold
    _io_iface->poke8(0x109, 0x23); // Rx1 Full/LMT Gain Index
    _io_iface->poke8(0x10A, 0x58); // Rx1 LPF Gain Index
    _io_iface->poke8(0x10B, 0x00); // Rx1 Digital Gain Index
    _io_iface->poke8(0x10C, 0x23); // Rx2 Full/LMT Gain Index
    _io_iface->poke8(0x10D, 0x18); // Rx2 LPF Gain Index
    _io_iface->poke8(0x10E, 0x00); // Rx2 Digital Gain Index
    _io_iface->poke8(0x114, 0x30); // Low Power Threshold
    _io_iface->poke8(0x11A, 0x27); // Initial LMT Gain Limit
    _io_iface->poke8(0x081, 0x00); // Tx Symbol Gain Control
}

/* Setup the RX or TX synthesizers.
 *
 * This setup depends on a fixed look-up table, which is stored in an
 * included header file. The table is indexed based on the passed VCO rate.
 */
void ad9361_device_t::_setup_synth(direction_t direction, double vcorate)
{
    /* The vcorates in the vco_index array represent lower boundaries for
     * rates. Once we find a match, we use that index to look-up the rest of
     * the register values in the LUT. */
    int vcoindex = 0;
    for (size_t i = 0; i < 53; i++) {
        vcoindex = i;
        if (vcorate > vco_index[i]) {
            break;
        }
    }
    if (vcoindex > 53)
        throw uhd::runtime_error("[ad9361_device_t] vcoindex > 53");

    /* Parse the values out of the LUT based on our calculated index... */
    boost::uint8_t vco_output_level = synth_cal_lut[vcoindex][0];
    boost::uint8_t vco_varactor = synth_cal_lut[vcoindex][1];
    boost::uint8_t vco_bias_ref = synth_cal_lut[vcoindex][2];
    boost::uint8_t vco_bias_tcf = synth_cal_lut[vcoindex][3];
    boost::uint8_t vco_cal_offset = synth_cal_lut[vcoindex][4];
    boost::uint8_t vco_varactor_ref = synth_cal_lut[vcoindex][5];
    boost::uint8_t charge_pump_curr = synth_cal_lut[vcoindex][6];
    boost::uint8_t loop_filter_c2 = synth_cal_lut[vcoindex][7];
    boost::uint8_t loop_filter_c1 = synth_cal_lut[vcoindex][8];
    boost::uint8_t loop_filter_r1 = synth_cal_lut[vcoindex][9];
    boost::uint8_t loop_filter_c3 = synth_cal_lut[vcoindex][10];
    boost::uint8_t loop_filter_r3 = synth_cal_lut[vcoindex][11];

    /* ... annnd program! */
    if (direction == RX) {
        _io_iface->poke8(0x23a, 0x40 | vco_output_level);
        _io_iface->poke8(0x239, 0xC0 | vco_varactor);
        _io_iface->poke8(0x242, vco_bias_ref | (vco_bias_tcf << 3));
        _io_iface->poke8(0x238, (vco_cal_offset << 3));
        _io_iface->poke8(0x245, 0x00);
        _io_iface->poke8(0x251, vco_varactor_ref);
        _io_iface->poke8(0x250, 0x70);
        _io_iface->poke8(0x23b, 0x80 | charge_pump_curr);
        _io_iface->poke8(0x23e, loop_filter_c1 | (loop_filter_c2 << 4));
        _io_iface->poke8(0x23f, loop_filter_c3 | (loop_filter_r1 << 4));
        _io_iface->poke8(0x240, loop_filter_r3);
    } else if (direction == TX) {
        _io_iface->poke8(0x27a, 0x40 | vco_output_level);
        _io_iface->poke8(0x279, 0xC0 | vco_varactor);
        _io_iface->poke8(0x282, vco_bias_ref | (vco_bias_tcf << 3));
        _io_iface->poke8(0x278, (vco_cal_offset << 3));
        _io_iface->poke8(0x285, 0x00);
        _io_iface->poke8(0x291, vco_varactor_ref);
        _io_iface->poke8(0x290, 0x70);
        _io_iface->poke8(0x27b, 0x80 | charge_pump_curr);
        _io_iface->poke8(0x27e, loop_filter_c1 | (loop_filter_c2 << 4));
        _io_iface->poke8(0x27f, loop_filter_c3 | (loop_filter_r1 << 4));
        _io_iface->poke8(0x280, loop_filter_r3);
    } else {
        throw uhd::runtime_error("[ad9361_device_t] [_setup_synth] INVALID_CODE_PATH");
    }
}


/* Tune the baseband VCO.
 *
 * This clock signal is what gets fed to the ADCs and DACs. This function is
 * not exported outside of this file, and is invoked based on the rate
 * fed to the public set_clock_rate function. */
double ad9361_device_t::_tune_bbvco(const double rate)
{
    UHD_LOG << boost::format("[ad9361_device_t::_tune_bbvco] rate=%.10f\n") % rate;

    /* Let's not re-tune to the same frequency over and over... */
    if (freq_is_nearly_equal(rate, _req_coreclk)) {
        return _adcclock_freq;
    }

    _req_coreclk = rate;

    const double fref = 40e6;
    const int modulus = 2088960;
    const double vcomax = 1430e6;
    const double vcomin = 672e6;
    double vcorate;
    int vcodiv;

    /* Iterate over VCO dividers until appropriate divider is found. */
    int i = 1;
    for (; i <= 6; i++) {
        vcodiv = 1 << i;
        vcorate = rate * vcodiv;

        if (vcorate >= vcomin && vcorate <= vcomax)
            break;
    }
    if (i == 7)
        throw uhd::runtime_error("[ad9361_device_t] _tune_bbvco: wrong vcorate");

    UHD_LOG << boost::format("[ad9361_device_t::_tune_bbvco] vcodiv=%d vcorate=%.10f\n") % vcodiv % vcorate;
    /* Fo = Fref * (Nint + Nfrac / mod) */
    int nint = vcorate / fref;
    UHD_LOG << boost::format("[ad9361_device_t::_tune_bbvco] (nint)=%.10f\n") % (vcorate / fref);
    int nfrac = lround(((vcorate / fref) - (double) nint) * (double) modulus);
    UHD_LOG << boost::format("[ad9361_device_t::_tune_bbvco] (nfrac)=%.10f\n") % (((vcorate / fref) - (double) nint) * (double) modulus);
    UHD_LOG << boost::format("[ad9361_device_t::_tune_bbvco] nint=%d nfrac=%d\n") % nint % nfrac;
    double actual_vcorate = fref
            * ((double) nint + ((double) nfrac / (double) modulus));

    /* Scale CP current according to VCO rate */
    const double icp_baseline = 150e-6;
    const double freq_baseline = 1280e6;
    double icp = icp_baseline * (actual_vcorate / freq_baseline);
    int icp_reg = (icp / 25e-6) - 1;

    _io_iface->poke8(0x045, 0x00);            // REFCLK / 1 to BBPLL
    _io_iface->poke8(0x046, icp_reg & 0x3F);  // CP current
    _io_iface->poke8(0x048, 0xe8);            // BBPLL loop filters
    _io_iface->poke8(0x049, 0x5b);            // BBPLL loop filters
    _io_iface->poke8(0x04a, 0x35);            // BBPLL loop filters

    _io_iface->poke8(0x04b, 0xe0);
    _io_iface->poke8(0x04e, 0x10);            // Max accuracy

    _io_iface->poke8(0x043, nfrac & 0xFF);         // Nfrac[7:0]
    _io_iface->poke8(0x042, (nfrac >> 8) & 0xFF);  // Nfrac[15:8]
    _io_iface->poke8(0x041, (nfrac >> 16) & 0xFF); // Nfrac[23:16]
    _io_iface->poke8(0x044, nint);                 // Nint

    _calibrate_lock_bbpll();

    _regs.bbpll = (_regs.bbpll & 0xF8) | i;

    _bbpll_freq = actual_vcorate;
    _adcclock_freq = (actual_vcorate / vcodiv);

    return _adcclock_freq;
}

/* This function re-programs all of the gains in the system.
 *
 * Because the gain values match to different gain indices based on the
 * current operating band, this function can be called to update all gain
 * settings to the appropriate index after a re-tune. */
void ad9361_device_t::_reprogram_gains()
{
    set_gain(RX, CHAIN_1,_rx1_gain);
    set_gain(RX, CHAIN_2,_rx2_gain);
    set_gain(TX, CHAIN_1,_tx1_gain);
    set_gain(TX, CHAIN_2,_tx2_gain);
}

/* This is the internal tune function, not available for a host call.
 *
 * Calculate the VCO settings for the requested frquency, and then either
 * tune the RX or TX VCO. */
double ad9361_device_t::_tune_helper(direction_t direction, const double value)
{
    /* The RFPLL runs from 6 GHz - 12 GHz */
    const double fref = 80e6;
    const int modulus = 8388593;
    const double vcomax = 12e9;
    const double vcomin = 6e9;
    double vcorate;
    int vcodiv;

    /* Iterate over VCO dividers until appropriate divider is found. */
    int i;
    for (i = 0; i <= 6; i++) {
        vcodiv = 2 << i;
        vcorate = value * vcodiv;
        if (vcorate >= vcomin && vcorate <= vcomax)
            break;
    }
    if (i == 7)
        throw uhd::runtime_error("[ad9361_device_t] RFVCO can't find valid VCO rate!");

    int nint = vcorate / fref;
    int nfrac = ((vcorate / fref) - nint) * modulus;

    double actual_vcorate = fref * (nint + (double) (nfrac) / modulus);
    double actual_lo = actual_vcorate / vcodiv;

    if (direction == RX) {

        _req_rx_freq = value;

        /* Set band-specific settings. */
        if (value < _client_params->get_band_edge(AD9361_RX_BAND0)) {
            _regs.inputsel = (_regs.inputsel & 0xC0) | 0x30;
        } else if ((value
                >= _client_params->get_band_edge(AD9361_RX_BAND0))
                && (value
                        < _client_params->get_band_edge(AD9361_RX_BAND1))) {
            _regs.inputsel = (_regs.inputsel & 0xC0) | 0x0C;
        } else if ((value
                >= _client_params->get_band_edge(AD9361_RX_BAND1))
                && (value <= 6e9)) {
            _regs.inputsel = (_regs.inputsel & 0xC0) | 0x03;
        } else {
            throw uhd::runtime_error("[ad9361_device_t] [_tune_helper] INVALID_CODE_PATH");
        }

        _io_iface->poke8(0x004, _regs.inputsel);

        /* Store vcodiv setting. */
        _regs.vcodivs = (_regs.vcodivs & 0xF0) | (i & 0x0F);

        /* Setup the synthesizer. */
        _setup_synth(RX, actual_vcorate);

        /* Tune!!!! */
        _io_iface->poke8(0x233, nfrac & 0xFF);
        _io_iface->poke8(0x234, (nfrac >> 8) & 0xFF);
        _io_iface->poke8(0x235, (nfrac >> 16) & 0xFF);
        _io_iface->poke8(0x232, (nint >> 8) & 0xFF);
        _io_iface->poke8(0x231, nint & 0xFF);
        _io_iface->poke8(0x005, _regs.vcodivs);

        /* Lock the PLL! */
        boost::this_thread::sleep(boost::posix_time::milliseconds(2));
        if ((_io_iface->peek8(0x247) & 0x02) == 0) {
            throw uhd::runtime_error("[ad9361_device_t] RX PLL NOT LOCKED");
        }

        _rx_freq = actual_lo;

        return actual_lo;

    } else {

        _req_tx_freq = value;

        /* Set band-specific settings. */
        if (value < _client_params->get_band_edge(AD9361_TX_BAND0)) {
            _regs.inputsel = _regs.inputsel | 0x40;
        } else if ((value
                >= _client_params->get_band_edge(AD9361_TX_BAND0))
                && (value <= 6e9)) {
            _regs.inputsel = _regs.inputsel & 0xBF;
        } else {
            throw uhd::runtime_error("[ad9361_device_t] [_tune_helper] INVALID_CODE_PATH");
        }

        _io_iface->poke8(0x004, _regs.inputsel);

        /* Store vcodiv setting. */
        _regs.vcodivs = (_regs.vcodivs & 0x0F) | ((i & 0x0F) << 4);

        /* Setup the synthesizer. */
        _setup_synth(TX, actual_vcorate);

        /* Tune it, homey. */
        _io_iface->poke8(0x273, nfrac & 0xFF);
        _io_iface->poke8(0x274, (nfrac >> 8) & 0xFF);
        _io_iface->poke8(0x275, (nfrac >> 16) & 0xFF);
        _io_iface->poke8(0x272, (nint >> 8) & 0xFF);
        _io_iface->poke8(0x271, nint & 0xFF);
        _io_iface->poke8(0x005, _regs.vcodivs);

        /* Lock the PLL! */
        boost::this_thread::sleep(boost::posix_time::milliseconds(2));
        if ((_io_iface->peek8(0x287) & 0x02) == 0) {
            throw uhd::runtime_error("[ad9361_device_t] TX PLL NOT LOCKED");
        }

        _tx_freq = actual_lo;

        return actual_lo;
    }
}

/* Configure the various clock / sample rates in the RX and TX chains.
 *
 * Functionally, this function configures AD9361's RX and TX rates. For
 * a requested TX & RX rate, it sets the interpolation & decimation filters,
 * and tunes the VCO that feeds the ADCs and DACs.
 */
double ad9361_device_t::_setup_rates(const double rate)
{
    /* If we make it into this function, then we are tuning to a new rate.
     * Store the new rate. */
    _req_clock_rate = rate;

    /* Set the decimation and interpolation values in the RX and TX chains.
     * This also switches filters in / out. Note that all transmitters and
     * receivers have to be turned on for the calibration portion of
     * bring-up, and then they will be switched out to reflect the actual
     * user-requested antenna selections. */
    int divfactor = 0;
    _tfir_factor = 0;
    if (rate < 0.33e6) {
        // RX1 + RX2 enabled, 3, 2, 2, 4
        _regs.rxfilt = B8(11101111);

        // TX1 + TX2 enabled, 3, 2, 2, 4
        _regs.txfilt = B8(11101111);

        divfactor = 48;
        _tfir_factor = 2;
    } else if (rate < 0.66e6) {
        // RX1 + RX2 enabled, 2, 2, 2, 4
        _regs.rxfilt = B8(11011111);

        // TX1 + TX2 enabled, 2, 2, 2, 4
        _regs.txfilt = B8(11011111);

        divfactor = 32;
        _tfir_factor = 2;
    } else if (rate <= 20e6) {
        // RX1 + RX2 enabled, 2, 2, 2, 2
        _regs.rxfilt = B8(11011110);

        // TX1 + TX2 enabled, 2, 2, 2, 2
        _regs.txfilt = B8(11011110);

        divfactor = 16;
        _tfir_factor = 2;
    } else if ((rate > 20e6) && (rate < 23e6)) {
        // RX1 + RX2 enabled, 3, 2, 2, 2
        _regs.rxfilt = B8(11101110);

        // TX1 + TX2 enabled, 3, 1, 2, 2
        _regs.txfilt = B8(11100110);

        divfactor = 24;
        _tfir_factor = 2;
    } else if ((rate >= 23e6) && (rate < 41e6)) {
        // RX1 + RX2 enabled, 2, 2, 2, 2
        _regs.rxfilt = B8(11011110);

        // TX1 + TX2 enabled, 1, 2, 2, 2
        _regs.txfilt = B8(11001110);

        divfactor = 16;
        _tfir_factor = 2;
    } else if ((rate >= 41e6) && (rate <= 56e6)) {
        // RX1 + RX2 enabled, 3, 1, 2, 2
        _regs.rxfilt = B8(11100110);

        // TX1 + TX2 enabled, 3, 1, 1, 2
        _regs.txfilt = B8(11100010);

        divfactor = 12;
        _tfir_factor = 2;
    } else if ((rate > 56e6) && (rate <= 61.44e6)) {
        // RX1 + RX2 enabled, 3, 1, 1, 2
        _regs.rxfilt = B8(11100010);

        // TX1 + TX2 enabled, 3, 1, 1, 1
        _regs.txfilt = B8(11100001);

        divfactor = 6;
        _tfir_factor = 1;
    } else {
        // should never get in here
        throw uhd::runtime_error("[ad9361_device_t] [_setup_rates] INVALID_CODE_PATH");
    }

    UHD_LOG << boost::format("[ad9361_device_t::_setup_rates] divfactor=%d\n") % divfactor;

    /* Tune the BBPLL to get the ADC and DAC clocks. */
    const double adcclk = _tune_bbvco(rate * divfactor);
    double dacclk = adcclk;

    /* The DAC clock must be <= 336e6, and is either the ADC clock or 1/2 the
     * ADC clock.*/
    if (adcclk > 336e6) {
        /* Make the DAC clock = ADC/2, and bypass the TXFIR. */
        _regs.bbpll = _regs.bbpll | 0x08;
        dacclk = adcclk / 2.0;
    } else {
        _regs.bbpll = _regs.bbpll & 0xF7;
    }

    /* Set the dividers / interpolators in AD9361. */
    _io_iface->poke8(0x002, _regs.txfilt);
    _io_iface->poke8(0x003, _regs.rxfilt);
    _io_iface->poke8(0x004, _regs.inputsel);
    _io_iface->poke8(0x00A, _regs.bbpll);

    UHD_LOG << boost::format("[ad9361_device_t::_setup_rates] adcclk=%f\n") % adcclk;
    _baseband_bw = (adcclk / divfactor);

    /*
     The Tx & Rx FIR calculate 16 taps per clock cycle. This limits the number of available taps to the ratio of DAC_CLK/ADC_CLK
     to the input data rate multiplied by 16. For example, if the input data rate is 25 MHz and DAC_CLK is 100 MHz,
     then the ratio of DAC_CLK to the input data rate is 100/25 or 4. In this scenario, the total number of taps available is 64.

     Also, whilst the Rx FIR filter always has memory available for 128 taps, the Tx FIR Filter can only support a maximum length of 64 taps
     in 1x interpolation mode, and 128 taps in 2x & 4x modes.
     */
    const size_t max_tx_taps = std::min<size_t>(
            std::min<size_t>((16 * (int)((dacclk / rate) + 0.5)), 128),
            (_tfir_factor == 1) ? 64 : 128);
    const size_t max_rx_taps = std::min<size_t>((16 * (size_t)((adcclk / rate) + 0.5)),
            128);

    const size_t num_tx_taps = get_num_taps(max_tx_taps);
    const size_t num_rx_taps = get_num_taps(max_rx_taps);

    _setup_tx_fir(num_tx_taps);
    _setup_rx_fir(num_rx_taps);

    return _baseband_bw;
}

/***********************************************************************
 * Publicly exported functions to host calls
 **********************************************************************/
void ad9361_device_t::initialize()
{
    boost::lock_guard<boost::recursive_mutex> lock(_mutex);

    /* Initialize shadow registers. */
    _regs.vcodivs = 0x00;
    _regs.inputsel = 0x30;
    _regs.rxfilt = 0x00;
    _regs.txfilt = 0x00;
    _regs.bbpll = 0x02;
    _regs.bbftune_config = 0x1e;
    _regs.bbftune_mode = 0x1e;

    /* Initialize private VRQ fields. */
    _rx_freq = 0.0;
    _tx_freq = 0.0;
    _req_rx_freq = 0.0;
    _req_tx_freq = 0.0;
    _baseband_bw = 0.0;
    _req_clock_rate = 0.0;
    _req_coreclk = 0.0;
    _bbpll_freq = 0.0;
    _adcclock_freq = 0.0;
    _rx_bbf_tunediv = 0;
    _curr_gain_table = 0;
    _rx1_gain = 0;
    _rx2_gain = 0;
    _tx1_gain = 0;
    _tx2_gain = 0;

    /* Reset the device. */
    _io_iface->poke8(0x000, 0x01);
    _io_iface->poke8(0x000, 0x00);
    boost::this_thread::sleep(boost::posix_time::milliseconds(20));

    /* There is not a WAT big enough for this. */
    _io_iface->poke8(0x3df, 0x01);

    _io_iface->poke8(0x2a6, 0x0e); // Enable master bias
    _io_iface->poke8(0x2a8, 0x0e); // Set bandgap trim

    /* Set RFPLL ref clock scale to REFCLK * 2 */
    _io_iface->poke8(0x2ab, 0x07);
    _io_iface->poke8(0x2ac, 0xff);

    /* Enable clocks. */
    switch (_client_params->get_clocking_mode()) {
    case AD9361_XTAL_N_CLK_PATH: {
        _io_iface->poke8(0x009, 0x17);
    } break;

    case AD9361_XTAL_P_CLK_PATH: {
        _io_iface->poke8(0x009, 0x07);
        _io_iface->poke8(0x292, 0x08);
        _io_iface->poke8(0x293, 0x80);
        _io_iface->poke8(0x294, 0x00);
        _io_iface->poke8(0x295, 0x14);
    } break;

    default:
        throw uhd::runtime_error("[ad9361_device_t] NOT IMPLEMENTED");
    }
    boost::this_thread::sleep(boost::posix_time::milliseconds(20));

    /* Tune the BBPLL, write TX and RX FIRS. */
    _setup_rates(50e6);

    /* Setup data ports (FDD dual port DDR):
     *      FDD dual port DDR CMOS no swap.
     *      Force TX on one port, RX on the other. */
    switch (_client_params->get_digital_interface_mode()) {
    case AD9361_DDR_FDD_LVCMOS: {
        _io_iface->poke8(0x010, 0xc8);
        _io_iface->poke8(0x011, 0x00);
        _io_iface->poke8(0x012, 0x02);
    } break;

    case AD9361_DDR_FDD_LVDS: {
        _io_iface->poke8(0x010, 0xcc);
        _io_iface->poke8(0x011, 0x00);
        _io_iface->poke8(0x012, 0x10);

        //LVDS Specific
        _io_iface->poke8(0x03C, 0x23);
        _io_iface->poke8(0x03D, 0xFF);
        _io_iface->poke8(0x03E, 0x0F);
    } break;

    default:
        throw uhd::runtime_error("[ad9361_device_t] NOT IMPLEMENTED");
    }

    /* Data delay for TX and RX data clocks */
    digital_interface_delays_t timing =
            _client_params->get_digital_interface_timing();
    boost::uint8_t rx_delays = ((timing.rx_clk_delay & 0xF) << 4)
            | (timing.rx_data_delay & 0xF);
    boost::uint8_t tx_delays = ((timing.tx_clk_delay & 0xF) << 4)
            | (timing.tx_data_delay & 0xF);
    _io_iface->poke8(0x006, rx_delays);
    _io_iface->poke8(0x007, tx_delays);

    /* Setup AuxDAC */
    _io_iface->poke8(0x018, 0x00); // AuxDAC1 Word[9:2]
    _io_iface->poke8(0x019, 0x00); // AuxDAC2 Word[9:2]
    _io_iface->poke8(0x01A, 0x00); // AuxDAC1 Config and Word[1:0]
    _io_iface->poke8(0x01B, 0x00); // AuxDAC2 Config and Word[1:0]
    _io_iface->poke8(0x022, 0x4A); // Invert Bypassed LNA
    _io_iface->poke8(0x023, 0xFF); // AuxDAC Manaul/Auto Control
    _io_iface->poke8(0x026, 0x00); // AuxDAC Manual Select Bit/GPO Manual Select
    _io_iface->poke8(0x030, 0x00); // AuxDAC1 Rx Delay
    _io_iface->poke8(0x031, 0x00); // AuxDAC1 Tx Delay
    _io_iface->poke8(0x032, 0x00); // AuxDAC2 Rx Delay
    _io_iface->poke8(0x033, 0x00); // AuxDAC2 Tx Delay

    /* Setup AuxADC */
    _io_iface->poke8(0x00B, 0x00); // Temp Sensor Setup (Offset)
    _io_iface->poke8(0x00C, 0x00); // Temp Sensor Setup (Temp Window)
    _io_iface->poke8(0x00D, 0x03); // Temp Sensor Setup (Periodic Measure)
    _io_iface->poke8(0x00F, 0x04); // Temp Sensor Setup (Decimation)
    _io_iface->poke8(0x01C, 0x10); // AuxADC Setup (Clock Div)
    _io_iface->poke8(0x01D, 0x01); // AuxADC Setup (Decimation/Enable)

    /* Setup control outputs. */
    _io_iface->poke8(0x035, 0x07);
    _io_iface->poke8(0x036, 0xFF);

    /* Setup GPO */
    _io_iface->poke8(0x03a, 0x27); //set delay register
    _io_iface->poke8(0x020, 0x00); // GPO Auto Enable Setup in RX and TX
    _io_iface->poke8(0x027, 0x03); // GPO Manual and GPO auto value in ALERT
    _io_iface->poke8(0x028, 0x00); // GPO_0 RX Delay
    _io_iface->poke8(0x029, 0x00); // GPO_1 RX Delay
    _io_iface->poke8(0x02A, 0x00); // GPO_2 RX Delay
    _io_iface->poke8(0x02B, 0x00); // GPO_3 RX Delay
    _io_iface->poke8(0x02C, 0x00); // GPO_0 TX Delay
    _io_iface->poke8(0x02D, 0x00); // GPO_1 TX Delay
    _io_iface->poke8(0x02E, 0x00); // GPO_2 TX Delay
    _io_iface->poke8(0x02F, 0x00); // GPO_3 TX Delay

    _io_iface->poke8(0x261, 0x00); // RX LO power
    _io_iface->poke8(0x2a1, 0x00); // TX LO power
    _io_iface->poke8(0x248, 0x0b); // en RX VCO LDO
    _io_iface->poke8(0x288, 0x0b); // en TX VCO LDO
    _io_iface->poke8(0x246, 0x02); // pd RX cal Tcf
    _io_iface->poke8(0x286, 0x02); // pd TX cal Tcf
    _io_iface->poke8(0x249, 0x8e); // rx vco cal length
    _io_iface->poke8(0x289, 0x8e); // rx vco cal length
    _io_iface->poke8(0x23b, 0x80); // set RX MSB?, FIXME 0x89 magic cp
    _io_iface->poke8(0x27b, 0x80); // "" TX //FIXME 0x88 see above
    _io_iface->poke8(0x243, 0x0d); // set rx prescaler bias
    _io_iface->poke8(0x283, 0x0d); // "" TX

    _io_iface->poke8(0x23d, 0x00); // Clear half VCO cal clock setting
    _io_iface->poke8(0x27d, 0x00); // Clear half VCO cal clock setting

    /* The order of the following process is EXTREMELY important. If the
     * below functions are modified at all, device initialization and
     * calibration might be broken in the process! */

    _io_iface->poke8(0x015, 0x04); // dual synth mode, synth en ctrl en
    _io_iface->poke8(0x014, 0x05); // use SPI for TXNRX ctrl, to ALERT, TX on
    _io_iface->poke8(0x013, 0x01); // enable ENSM
    boost::this_thread::sleep(boost::posix_time::milliseconds(1));

    _calibrate_synth_charge_pumps();

    _tune_helper(RX, 800e6);
    _tune_helper(TX, 850e6);

    _program_mixer_gm_subtable();
    _program_gain_table();
    _setup_gain_control();

    _calibrate_baseband_rx_analog_filter();
    _calibrate_baseband_tx_analog_filter();
    _calibrate_rx_TIAs();
    _calibrate_secondary_tx_filter();

    _setup_adc();

    _calibrate_tx_quadrature();
    _calibrate_rx_quadrature();

    // cals done, set PPORT config
    switch (_client_params->get_digital_interface_mode()) {
    case AD9361_DDR_FDD_LVCMOS: {
        _io_iface->poke8(0x012, 0x02);
    } break;

    case AD9361_DDR_FDD_LVDS: {
        _io_iface->poke8(0x012, 0x10);
    } break;

    default:
        throw uhd::runtime_error("[ad9361_device_t] NOT IMPLEMENTED");
    }

    _io_iface->poke8(0x013, 0x01); // Set ENSM FDD bit
    _io_iface->poke8(0x015, 0x04); // dual synth mode, synth en ctrl en

    /* Default TX attentuation to 10dB on both TX1 and TX2 */
    _io_iface->poke8(0x073, 0x00);
    _io_iface->poke8(0x074, 0x00);
    _io_iface->poke8(0x075, 0x00);
    _io_iface->poke8(0x076, 0x00);

    /* Setup RSSI Measurements */
    _io_iface->poke8(0x150, 0x0E); // RSSI Measurement Duration 0, 1
    _io_iface->poke8(0x151, 0x00); // RSSI Measurement Duration 2, 3
    _io_iface->poke8(0x152, 0xFF); // RSSI Weighted Multiplier 0
    _io_iface->poke8(0x153, 0x00); // RSSI Weighted Multiplier 1
    _io_iface->poke8(0x154, 0x00); // RSSI Weighted Multiplier 2
    _io_iface->poke8(0x155, 0x00); // RSSI Weighted Multiplier 3
    _io_iface->poke8(0x156, 0x00); // RSSI Delay
    _io_iface->poke8(0x157, 0x00); // RSSI Wait
    _io_iface->poke8(0x158, 0x0D); // RSSI Mode Select
    _io_iface->poke8(0x15C, 0x67); // Power Measurement Duration

    /* Turn on the default RX & TX chains. */
    set_active_chains(true, false, false, false);

    /* Set TXers & RXers on (only works in FDD mode) */
    _io_iface->poke8(0x014, 0x21);
}


/* This function sets the RX / TX rate between AD9361 and the FPGA, and
 * thus determines the interpolation / decimation required in the FPGA to
 * achieve the user's requested rate.
 *
 * This is the only clock setting function that is exposed to the outside. */
double ad9361_device_t::set_clock_rate(const double req_rate)
{
    boost::lock_guard<boost::recursive_mutex> lock(_mutex);

    if (req_rate > 61.44e6) {
        throw uhd::runtime_error("[ad9361_device_t] Requested master clock rate outside range");
    }

    UHD_LOG << boost::format("[ad9361_device_t::set_clock_rate] req_rate=%.10f\n") % req_rate;

    /* UHD has a habit of requesting the same rate like four times when it
     * starts up. This prevents that, and any bugs in user code that request
     * the same rate over and over. */
    if (freq_is_nearly_equal(req_rate, _req_clock_rate)) {
        return _baseband_bw;
    }

    /* We must be in the SLEEP / WAIT state to do this. If we aren't already
     * there, transition the ENSM to State 0. */
    boost::uint8_t current_state = _io_iface->peek8(0x017) & 0x0F;
    switch (current_state) {
    case 0x05:
        /* We are in the ALERT state. */
        _io_iface->poke8(0x014, 0x21);
        boost::this_thread::sleep(boost::posix_time::milliseconds(5));
        _io_iface->poke8(0x014, 0x00);
        break;

    case 0x0A:
        /* We are in the FDD state. */
        _io_iface->poke8(0x014, 0x00);
        break;

    default:
        throw uhd::runtime_error("[ad9361_device_t] [set_clock_rate:1] AD9361 in unknown state");
        break;
    };

    /* Store the current chain / antenna selections so that we can restore
     * them at the end of this routine; all chains will be enabled from
     * within setup_rates for calibration purposes. */
    boost::uint8_t orig_tx_chains = _regs.txfilt & 0xC0;
    boost::uint8_t orig_rx_chains = _regs.rxfilt & 0xC0;

    /* Call into the clock configuration / settings function. This is where
     * all the hard work gets done. */
    double rate = _setup_rates(req_rate);

    UHD_LOG << boost::format("[ad9361_device_t::set_clock_rate] rate=%.10f\n") % rate;

    /* Transition to the ALERT state and calibrate everything. */
    _io_iface->poke8(0x015, 0x04); //dual synth mode, synth en ctrl en
    _io_iface->poke8(0x014, 0x05); //use SPI for TXNRX ctrl, to ALERT, TX on
    _io_iface->poke8(0x013, 0x01); //enable ENSM
    boost::this_thread::sleep(boost::posix_time::milliseconds(1));

    _calibrate_synth_charge_pumps();

    _tune_helper(RX, _rx_freq);
    _tune_helper(TX, _tx_freq);

    _program_mixer_gm_subtable();
    _program_gain_table();
    _setup_gain_control();
    _reprogram_gains();

    _calibrate_baseband_rx_analog_filter();
    _calibrate_baseband_tx_analog_filter();
    _calibrate_rx_TIAs();
    _calibrate_secondary_tx_filter();

    _setup_adc();

    _calibrate_tx_quadrature();
    _calibrate_rx_quadrature();

    // cals done, set PPORT config
    switch (_client_params->get_digital_interface_mode()) {
        case AD9361_DDR_FDD_LVCMOS: {
            _io_iface->poke8(0x012, 0x02);
        }break;

        case AD9361_DDR_FDD_LVDS: {
            _io_iface->poke8(0x012, 0x10);
        }break;

        default:
        throw uhd::runtime_error("[ad9361_device_t] NOT IMPLEMENTED");
    }
    _io_iface->poke8(0x013, 0x01); // Set ENSM FDD bit
    _io_iface->poke8(0x015, 0x04); // dual synth mode, synth en ctrl en

    /* End the function in the same state as the entry state. */
    switch (current_state) {
    case 0x05:
        /* We are already in ALERT. */
        break;

    case 0x0A:
        /* Transition back to FDD, and restore the original antenna
         * / chain selections. */
        _regs.txfilt = (_regs.txfilt & 0x3F) | orig_tx_chains;
        _regs.rxfilt = (_regs.rxfilt & 0x3F) | orig_rx_chains;

        _io_iface->poke8(0x002, _regs.txfilt);
        _io_iface->poke8(0x003, _regs.rxfilt);
        _io_iface->poke8(0x014, 0x21);
        break;

    default:
        throw uhd::runtime_error("[ad9361_device_t] [set_clock_rate:2] AD9361 in unknown state");
        break;
    };

    return rate;
}


/* Set which of the four TX / RX chains provided by AD9361 are active.
 *
 * AD9361 provides two sets of chains, Side A and Side B. Each side
 * provides one TX antenna, and one RX antenna. The B200 maintains the USRP
 * standard of providing one antenna connection that is both TX & RX, and
 * one that is RX-only - for each chain. Thus, the possible antenna and
 * chain selections are:
 *
 *  B200 Antenna    AD9361 Side       AD9361 Chain
 *  -------------------------------------------------------------------
 *  TX / RX1        Side A              TX1 (when switched to TX)
 *  TX / RX1        Side A              RX1 (when switched to RX)
 *  RX1             Side A              RX1
 *
 *  TX / RX2        Side B              TX2 (when switched to TX)
 *  TX / RX2        Side B              RX2 (when switched to RX)
 *  RX2             Side B              RX2
 */
void ad9361_device_t::set_active_chains(bool tx1, bool tx2, bool rx1, bool rx2)
{
    boost::lock_guard<boost::recursive_mutex> lock(_mutex);

    /* Clear out the current active chain settings. */
    _regs.txfilt = _regs.txfilt & 0x3F;
    _regs.rxfilt = _regs.rxfilt & 0x3F;

    /* Turn on the different chains based on the passed parameters. */
    if (tx1) {
        _regs.txfilt = _regs.txfilt | 0x40;
    }
    if (tx2) {
        _regs.txfilt = _regs.txfilt | 0x80;
    }
    if (rx1) {
        _regs.rxfilt = _regs.rxfilt | 0x40;
    }
    if (rx2) {
        _regs.rxfilt = _regs.rxfilt | 0x80;
    }

    /* Check for FDD state */
    boost::uint8_t set_back_to_fdd = 0;
    boost::uint8_t ensm_state = _io_iface->peek8(0x017) & 0x0F;
    if (ensm_state == 0xA)   // FDD
            {
        /* Put into ALERT state (via the FDD flush state). */
        _io_iface->poke8(0x014, 0x01);
        set_back_to_fdd = 1;
    }

    /* Wait for FDD flush state to complete (if necessary) */
    while (ensm_state == 0xA || ensm_state == 0xB)
        ensm_state = _io_iface->peek8(0x017) & 0x0F;

    /* Turn on / off the chains. */
    _io_iface->poke8(0x002, _regs.txfilt);
    _io_iface->poke8(0x003, _regs.rxfilt);

    /* Put back into FDD state if necessary */
    if (set_back_to_fdd)
        _io_iface->poke8(0x014, 0x21);
}

/* Tune the RX or TX frequency.
 *
 * This is the publicly-accessible tune function. It makes sure the tune
 * isn't a redundant request, and if not, passes it on to the class's
 * internal tune function.
 *
 * After tuning, it runs any appropriate calibrations. */
double ad9361_device_t::tune(direction_t direction, const double value)
{
    boost::lock_guard<boost::recursive_mutex> lock(_mutex);

    if (direction == RX) {
        if (freq_is_nearly_equal(value, _req_rx_freq)) {
            return _rx_freq;
        }

    } else if (direction == TX) {
        if (freq_is_nearly_equal(value, _req_tx_freq)) {
            return _tx_freq;
        }

    } else {
        throw uhd::runtime_error("[ad9361_device_t] [tune] INVALID_CODE_PATH");
    }

    /* If we aren't already in the ALERT state, we will need to return to
     * the FDD state after tuning. */
    int not_in_alert = 0;
    if ((_io_iface->peek8(0x017) & 0x0F) != 5) {
        /* Force the device into the ALERT state. */
        not_in_alert = 1;
        _io_iface->poke8(0x014, 0x01);
    }

    /* Tune the RF VCO! */
    double tune_freq = _tune_helper(direction, value);

    /* Run any necessary calibrations / setups */
    if (direction == RX) {
        _program_gain_table();
    }

    /* Update the gain settings. */
    _reprogram_gains();

    /* Run the calibration algorithms. */
    _calibrate_tx_quadrature();
    _calibrate_rx_quadrature();

    /* If we were in the FDD state, return it now. */
    if (not_in_alert) {
        _io_iface->poke8(0x014, 0x21);
    }

    return tune_freq;
}

/* Set the gain of RX1, RX2, TX1, or TX2.
 *
 * Note that the 'value' passed to this function is the actual gain value,
 * _not_ the gain index. This is the opposite of the eval software's GUI!
 * Also note that the RX chains are done in terms of gain, and the TX chains
 * are done in terms of attenuation. */
double ad9361_device_t::set_gain(direction_t direction, chain_t chain, const double value)
{
    boost::lock_guard<boost::recursive_mutex> lock(_mutex);

    if (direction == RX) {
        /* Indexing the gain tables requires an offset from the requested
         * amount of total gain in dB:
         *      < 1300MHz: dB + 5
         *      >= 1300MHz and < 4000MHz: dB + 3
         *      >= 4000MHz and <= 6000MHz: dB + 14
         */
        int gain_offset = 0;
        if (_rx_freq < 1300e6) {
            gain_offset = 5;
        } else if (_rx_freq < 4000e6) {
            gain_offset = 3;
        } else {
            gain_offset = 14;
        }

        int gain_index = value + gain_offset;

        /* Clip the gain values to the proper min/max gain values. */
        if (gain_index > 76)
            gain_index = 76;
        if (gain_index < 0)
            gain_index = 0;

        if (chain == CHAIN_1) {
            _rx1_gain = value;
            _io_iface->poke8(0x109, gain_index);
        } else {
            _rx2_gain = value;
            _io_iface->poke8(0x10c, gain_index);
        }

        return gain_index - gain_offset;
    } else {
        /* Setting the below bits causes a change in the TX attenuation word
         * to immediately take effect. */
        _io_iface->poke8(0x077, 0x40);
        _io_iface->poke8(0x07c, 0x40);

        /* Each gain step is -0.25dB. Calculate the attenuation necessary
         * for the requested gain, convert it into gain steps, then write
         * the attenuation word. Max gain (so zero attenuation) is 89.75. */
        double atten = AD9361_MAX_GAIN - value;
        int attenreg = atten * 4;
        if (chain == CHAIN_1) {
            _tx1_gain = value;
            _io_iface->poke8(0x073, attenreg & 0xFF);
            _io_iface->poke8(0x074, (attenreg >> 8) & 0x01);
        } else {
            _tx2_gain = value;
            _io_iface->poke8(0x075, attenreg & 0xFF);
            _io_iface->poke8(0x076, (attenreg >> 8) & 0x01);
        }
        return AD9361_MAX_GAIN - ((double) (attenreg) / 4);
    }
}

void ad9361_device_t::output_test_tone()
{
    boost::lock_guard<boost::recursive_mutex> lock(_mutex);
    /* Output a 480 kHz tone at 800 MHz */
    _io_iface->poke8(0x3F4, 0x0B);
    _io_iface->poke8(0x3FC, 0xFF);
    _io_iface->poke8(0x3FD, 0xFF);
    _io_iface->poke8(0x3FE, 0x3F);
}

void ad9361_device_t::data_port_loopback(const bool loopback_enabled)
{
    boost::lock_guard<boost::recursive_mutex> lock(_mutex);
    _io_iface->poke8(0x3F5, (loopback_enabled ? 0x01 : 0x00));
}

}}
