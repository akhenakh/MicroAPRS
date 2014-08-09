//////////////////////////////////////////////////////
// First things first, all the includes we need     //
//////////////////////////////////////////////////////

#include "hardware.h"		// We need the header for this code
#include "afsk.h"           // We also need to know about the AFSK modem

#include <cpu/irq.h>        // Interrupt functions from BertOS

#include <avr/io.h>         // AVR IO functions from BertOS
#include <avr/interrupt.h>  // AVR interrupt functions from BertOS

// A reference to our modem "object"
static Afsk *modem;

//////////////////////////////////////////////////////
// And now for the actual hardware functions        //
//////////////////////////////////////////////////////

// M1 correction = 9500
// M2 correction = 40000
#define FREQUENCY_CORRECTION 0

// This function initializes the ADC and configures
// it the way we need.
void hw_afsk_adcInit(int ch, Afsk *_modem)
{
    // Store a reference to our modem "object"
    modem = _modem;

    // Also make sure that we are not trying to use
    // a pin that can't be used for analog input
    ASSERT(ch <= 7);

    // We need a timer to control how often our sampling functions
    // should run. To do this we will need to change some registers.
    // First we do some configuration on the Timer/Counter Control
    // Register 1, aka Timer1.
    //
    // The following bits are set:
    // CS10: ClockSource 10, sets no prescaler on the clock,
    // meaning it will run at the same speed as the CPU, ie 16MHz
    // WGM13 and WGM12 together enables "Timer Mode 12", which
    // is Clear Timer on Compare, compare set to TOP, and the
    // source for the TOP value is ICR1 (Input Capture Register1).
    // TOP means that we specify a maximum value for the timer, and
    // once that value is reached, an interrupt will be triggered.
    // The timer will then start from zero again. As just noted,
    // the place we specify this value is in the ICR1 register.
    TCCR1A = 0;                                    
    TCCR1B = BV(CS10) | BV(WGM13) | BV(WGM12);

    // We now set the ICR1 register to what count value we want to
    // reset (and thus trigger the interrupt) at.
    // Since the timer is running at 16MHz, the counter will be
    // incremented 16 million times each second, and we want the
    // interrupt to trigger 9600 times each second. The formula for
    // calculating the value of ICR1 (the TOP value) is:
    //    (CPUClock / Prescaler) / desired frequency - 1
    // So that's what well put in this register to set up our
    // 9.6KHz sampling rate. Note that we can also specify a clock
    // correction to this calculation. If you measure your processors
    // actual clock speed to 16.095MHz, define FREQUENCY_CORRECTION
    // as 9500, and the actual sampling (and this modulation and
    // demodulation) will be much closer to an actual 9600 Hz.
    // No crystals are perfect though, and will also drift with
    // temperature variations, but if you have a board with a
    // crystal that is way off frequency, this can help alot.
    ICR1 = (((CPU_FREQ+FREQUENCY_CORRECTION)) / 9600) - 1;

    // Set reference to AVCC (5V), select pin
    // Set the ADMUX register. The first part (BV(REFS0)) sets
    // the reference voltage to VCC (5V), and the next selects
    // the ADC channel (basically what pin we are capturing on)
    ADMUX = BV(REFS0) | ch;

    DDRC &= ~BV(ch);    // Set the selected channel (pin) to input
    PORTC &= ~BV(ch);   // Initialize the selected pin to LOW
    DIDR0 |= BV(ch);    // Disable the Digital Input Buffer on selected pin

    // Now a little more configuration to get the ADC working
    // the way we want
    ADCSRB =    BV(ADTS2) | // Setting these three on (1-1-1) sets the ADC to
                BV(ADTS1) | // "Timer1 capture event". That means we can declare
                BV(ADTS0);  // an ISR in the ADC Vector, that will then get called
                            // everytime the ADC has a sample ready, which will
                            // happen at the 9.6Khz sampling rate we set up earlier
                
    ADCSRA =    BV(ADEN) |  // ADC Enable - Yes, we need to turn it on :)
                BV(ADSC) |  // ADC Start Converting - Tell it to start doing conversions
                BV(ADATE)|  // Enable autotriggering - Enables the autotrigger on complete
                BV(ADIE) |  // ADC Interrupt enable - Enables an interrupt to be called
                BV(ADPS2);  // Enable prescaler flag 2 (1-0-0 = division by 16 = 1MHz)
                            // This sets the ADC to run at 1MHz. This is out of spec,
                            // Since it's normal operating range is only up to 200KHz.
                            // But don't worry, it's not dangerous! I promise it wont
                            // blow up :) There is a downside to running at this speed
                            // though, hence the "out of spec", which is that we get
                            // a much lower resolution on the output. In this case,
                            // it's not a problem though, since we don't need the full
                            // 10-bit resolution, so we'll take fast and less precise!
}


// This declares the Interrupt Service routine that will
// get called everytime the ADC finishes taking a sample.
// What actually happens here is that we take a piece of
// code, store it somewhere in memory, and then put the
// address of that "somewhere" into the Interrupt Vector
// Table of the processor, in this case the position 
// "ADC_vect". This lets the processor know what to do
// when all the timing and configuration we just set up
// finally* ends up triggering the interrupt.
bool hw_ptt_on;
bool hw_afsk_dac_isr;
DECLARE_ISR(ADC_vect) {
    TIFR1 = BV(ICF1);

    // Call the routine for analysing the captured sample
    // Notice that we read the ADC sample, and then bitshift
    // by two places to the right, effectively eliminating
    // two bits of precision. But we didn't have those
    // anyway, because the ADC is running at high speed.
    // We then subtract 128 from the value, to get the
    // representation to match an AC waveform. We need to
    // do this because the AC waveform (from the audio input)
    // is biased by +2.5V, which is nessecary, since the ADC
    // can't read negative voltages. By doing this simple
    // math, we bring it back to an AC representation
    // we can do further calculations on.
    afsk_adc_isr(modem, ((int16_t)((ADC) >> 2) - 128));

    // We also need to check if we're supposed to spit
    // out some modulated data to the DAC.
    if (hw_afsk_dac_isr) {
        // If there is, it's easy to actually do so. We
        // calculate what the sample should be in the
        // DAC ISR, and apply the bitmask 11110000. This
        // simoultaneously spits out our 4-bit digital
        // sample to the four pins connected to our DAC
        // circuit, which then converts it to an analog
        // waveform. The reason for the " | BV(3)" is that
        // we also need to trigger another pin controlled
        // by the PORTD register. This is the PTT pin
        // which tells the radio to open it transmitter.
        PORTD = (afsk_dac_isr(modem) & 0xF0) | BV(3); 
    } else {
        // If we're not supposed to transmit anything, we
        // keep quiet by continously sending 128, which
        // when converted to an AC waveform by the DAC,
        // equates to a steady, unchanging 0 volts.
        if (hw_ptt_on) {
            PORTD = 136;
        } else {
            PORTD = 128;
        }
    }
        
}


// (*) "finally" is probably the wrong description here.
// "All the f'ing time" is probably more accurate :)
// but it felt like it was a long way down here,
// writing all the explanations. I think this is a
// nice testament to how efficient and smart these
// processors are. The actual code to set up what
// took a long time to explain, is really very short.