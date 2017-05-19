/*
 
   <rpitx is a software which use the GPIO of Raspberry Pi to transmit HF>

    Copyright (C) 2015  Evariste COURJAUD F5OEO (evaristec@gmail.com)

    Transmitting on HF band is surely not permitted without license (Hamradio for example).
    Usage of this software is not the responsability of the author.
	
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
   
   Thanks to first test of RF with Pifm by Oliver Mattos and Oskar Weigl 	
   INSPIRED BY THE IMPLEMENTATION OF PIFMDMA by Richard Hirst <richardghirst@gmail.com>  December 2012
   Helped by a code fragment by PE1NNZ (http://pe1nnz.nl.eu.org/2013/05/direct-ssb-generation-on-pll.html)
 */

/* ================== TODO =====================	
Optimize CPU on PWMFrequency
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>

//#include "utils.h"
#include "mailbox.h"
#include <getopt.h>
#include <termios.h>		//Used for UART
#include "RpiGpio.h"
#include "RpiDma.h"
#include <pthread.h>

#include "RpiTx.h"

#include <sys/prctl.h>
#include <getopt.h>
#include <sys/timex.h>

#define AMP_BYPAD

//Minimum Time in us to sleep
#define KERNEL_GRANULARITY 20000 

#define SCHED_PRIORITY 30 //Linux scheduler priority. Higher = more realtime


#define PROGRAM_VERSION "0.2"

#define PLL_FREQ_500MHZ		500000000	// PLLD is running at 500MHz
#define PLL_500MHZ 		0x6

//#define PLL_FREQ_1GHZ             1000000000	//PLLC = 1GHZ
//#define PLL_1GHZ			0x5

#define PLL_FREQ_1GHZ             1000000000	//PLL = 1GHZ
#define PLL_1GHZ			0x6     //PLLD = 1GHZ ONLY AFTER APLYINg DT-BLOB.BIN !!!! WARNING !!!


#define PLLFREQ_192             19200000	//PLLA = 19.2MHZ
#define PLL_192			0x1

#define HEADER_SIZE 44

// DMA TIMING : depends on Pi Model : Calibration is better
#define FREQ_DELAY_TIME 0
int FREQ_MINI_TIMING=157;
int PWMF_MARGIN = 1120; //A Margin for now at 1us with PCM ->OK
int globalppmpll=0;

typedef unsigned char 	uchar;      // 8 bit
typedef unsigned short	uint16;     // 16 bit
typedef unsigned int	uint;       // 32 bits

//F5OEO Variable
uint32_t PllFreq500MHZ;
uint32_t PllFreq1GHZ;
uint32_t PllFreq19MHZ;

uint32_t PllUsed;
char PllNumber;
double TuneFrequency=62500000;
unsigned char FreqDivider=2;
int DmaSampleBurstSize=1000;
int NUM_SAMPLES=NUM_SAMPLES_MAX;
int Randomize=0;

uint32_t GlobalTabPwmFrequency[50];

//End F5OEO

char EndOfApp=0;
unsigned char loop_mode_flag=0;
char *FileName = 0;
int FileInHandle = -1; //Handle in Transport Stream File
int useStdin = 0;

static void udelay(int us)
{
	struct timespec ts = { 0, us * 1000 };

	nanosleep(&ts, NULL);
}

static void stop_dma(void)
{
	
	if (FileInHandle != -1) {
		close(FileInHandle);
		FileInHandle = -1;
	}
	if (dma_reg) {
		//Stop Main DMA
		//STop DMA
		dma_reg[DMA_CS+DMA_CHANNEL*0x40] |= DMA_CS_ABORT;
		udelay(100);
		dma_reg[DMA_CS+DMA_CHANNEL*0x40]&= ~DMA_CS_ACTIVE;
		dma_reg[DMA_CS+DMA_CHANNEL*0x40] |= DMA_CS_RESET;
		udelay(100);
		

		//printf("Reset DMA Done\n");
		clk_reg[GPCLK_CNTL] = 0x5A << 24  | 0 << 9 | 1 << 4 | 6; //NO MASH !!!
		udelay(500);
		gpio_reg[GPFSEL0] = (gpio_reg[GPFSEL0] & ~(7 << 12)) | (0 << 12); //DISABLE CLOCK -
		clk_reg[PWMCLK_CNTL] = 0x5A000006 | (0 << 9) ;
		udelay(500);	
		clk_reg[PCMCLK_CNTL] = 0x5A000006;	
		udelay(500);
		//printf("Resetpcm Done\n");
		pwm_reg[PWM_DMAC] = 0;
		udelay(100);
		pwm_reg[PWM_CTL] = PWMCTL_CLRF;
		udelay(100);
		//printf("Reset pwm Done\n");
	}
	if (mbox.virt_addr != NULL) {
		unmapmem(mbox.virt_addr, NUM_PAGES * PAGE_SIZE);
		//printf("Unmapmem Done\n");
		mem_unlock(mbox.handle, mbox.mem_ref);
		//printf("Unmaplock Done\n");
		mem_free(mbox.handle, mbox.mem_ref);
		//printf("Unmapfree Done\n");
	}
}

static void terminate(int dummy)
{
	stop_dma();
	//munmap(virtbase,NUM_PAGES * PAGE_SIZE); 
	printf("END OF PiTx\n");
	exit(1);
}

static void fatal(char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	terminate(0);
}

void setSchedPriority(int priority) 
{
	//In order to get the best timing at a decent queue size, we want the kernel to avoid interrupting us for long durations.
	//This is done by giving our process a high priority. Note, must run as super-user for this to work.
	struct sched_param sp;
	int ret;
	
	sp.sched_priority=priority;
	if ((ret = pthread_setschedparam(pthread_self(), SCHED_RR, &sp))) {
		printf("Warning: pthread_setschedparam (increase thread priority) returned non-zero: %i\n", ret);
	}

}

uint32_t DelayFromSampleRate=0;
int Instrumentation=0;
int UsePCMClk=0;
uint32_t Originfsel=0;

int SetupGpioClock(uint32_t SymbolRate,double TuningFrequency)
{
	char MASH=1;
	 	
	if(UsePCMClk==0) TuningFrequency=TuningFrequency*2;	
	if((TuningFrequency>=100e6)&&(TuningFrequency<=150e6))
	{
		MASH=2;
	}
	if(TuningFrequency<100e6)
	{
		MASH=3;
	}
	
	printf("MASH %d Freq PLL# %d\n",MASH,PllNumber);
	Originfsel=gpio_reg[GPFSEL0]; // Warning carefull if FSEL is used after !!!!!!!!!!!!!!!!!!!!
	if(UsePCMClk==1)
			gpio_reg[GPFSEL0] = (Originfsel & ~(7 << 12)) | (4 << 12); //ENABLE CLOCK ON GPIO CLK

		
	// ------------------- MAKE MAX OUTPUT CURRENT FOR GPIO -----------------------
	char OutputPower=7;
	pad_gpios_reg[PADS_GPIO_0] = 0x5a000000 + (OutputPower&0x7) + (1<<4) + (0<<3); // Set output power for I/Q GPIO18/GPIO19 

#ifdef USE_PCM
	//------------------- Init PCM ------------------
	pcm_reg[PCM_CS_A] = 1;				// Disable Rx+Tx, Enable PCM block
	udelay(100);
	clk_reg[PCMCLK_CNTL] = 0x5A000000|PLL_1GHZ;		// Source=PLLC (1GHHz) STOP PLL
	udelay(1000);
	static uint32_t FreqDividerPCM;
	static uint32_t FreqFractionnalPCM;
	int NbStepPCM = 25; // Should not exceed 1000 : 
	
	FreqDividerPCM=(int) ((double)PllFreq1GHZ/(SymbolRate*NbStepPCM/**PwmNumberStep*/));
	FreqFractionnalPCM=4096.0 * (((double)PllFreq1GHZ/(SymbolRate*NbStepPCM/**PwmNumberStep*/))-FreqDividerPCM);
	
	printf("SampleRate=%d\n",SymbolRate);
	if((FreqDividerPCM>4096)||(FreqDividerPCM<2)) printf("Warning : SampleRate is not valid\n"); 
	clk_reg[PCMCLK_DIV] = 0x5A000000 | ((FreqDividerPCM)<<12) | FreqFractionnalPCM;
	udelay(1000);
	printf("Div PCM %d FracPCM %d\n",FreqDividerPCM,FreqFractionnalPCM);
	
	 DelayFromSampleRate=(1e9/(SymbolRate));
			
	pcm_reg[PCM_TXC_A] = 0<<31 | 1<<30 | 0<<20 | 0<<16; // 1 channel, 8 bits
	udelay(100);
	
	printf("Nb PCM STEP (<1000):%d\n",NbStepPCM);
	pcm_reg[PCM_MODE_A] = (NbStepPCM-1)<<10; // SHOULD NOT EXCEED 1000 !!!
	udelay(100);
	pcm_reg[PCM_CS_A] |= 1<<4 | 1<<3;		// Clear FIFOs
	udelay(100);
	pcm_reg[PCM_DREQ_A] = 64<<24 | /*64<<8 |*/ 64<<8 ;		//TX Fifo PCM=64 DMA Req when one slot is free?
	udelay(100);
	pcm_reg[PCM_CS_A] |= 1<<9;			// Enable DMA
	udelay(1000);
	clk_reg[PCMCLK_CNTL] = 0x5A000010 |(1 << 9)| PLL_1GHZ	/*PLL_1GHZ*/;		// Source=PLLC and enable
	udelay(100);
	pcm_reg[PCM_CS_A] |= 1<<2; //START TX PCM
#endif
	// FIN PCM

	//INIT PWM in Serial Mode : WE USE PWM OUPUT
	if(UsePCMClk==0)
	{
		gpioSetMode(18, 2); /* set to ALT5, PWM1 : RF On PIN */	

		pwm_reg[PWM_CTL] = 0;
		clk_reg[PWMCLK_CNTL] = 0x5A000000 | (MASH << 9) |PllNumber/*PLL_1GHZ*/ ;
		udelay(300);
		clk_reg[PWMCLK_DIV] = 0x5A000000 | (2<<12); //WILL BE UPDATED BY DMA
		udelay(300);
		clk_reg[PWMCLK_CNTL] = 0x5A000010 | (MASH << 9) | PllNumber /*PLL_1GHZ*/; //MASH3 : A TESTER SI MIEUX en MASH1
		//MASH 3 doesnt seem work above 80MHZ, back to MASH1
		pwm_reg[PWM_RNG1] = 32;// 250 -> 8KHZ
		udelay(100);
		pwm_reg[PWM_RNG2] = 32;// 32 Mandatory for Serial Mode without gap
	
		pwm_reg[PWM_FIFO]=0xAAAAAAAA;
		pwm_reg[PWM_DMAC] = PWMDMAC_ENAB | PWMDMAC_THRSHLD;
		udelay(100);
		pwm_reg[PWM_CTL] = PWMCTL_CLRF;
		udelay(100);
		pwm_reg[PWM_CTL] =   PWMCTL_USEF1| PWMCTL_MODE1| PWMCTL_PWEN1|PWMCTL_RPTL1; //PWM0 in Repeat mode 
	}
	// FIN INIT PWM

	//******************* INIT CLK MODE : WE OUTPUT CLK INSTEAD OF PWM OUTPUT
	if(UsePCMClk==1)
	{
		clk_reg[GPCLK_CNTL] = 0x5A000000 | (MASH << 9) |PllNumber/*PLL_1GHZ*/ ;
		udelay(300);
		clk_reg[GPCLK_DIV] = 0x5A000000 | (2<<12); //WILL BE UPDATED BY DMA !! CAREFUL NOT DIVIDE BY 2 LIKE PWM
		udelay(300);
		clk_reg[GPCLK_CNTL] = 0x5A000010 | (MASH << 9) | PllNumber /*PLL_1GHZ*/; //MASH3 : A TESTER SI MIEUX en MASH1
	}


	ctl = (struct control_data_s *)virtbase; // Struct ctl is mapped to the memory allocated by RpiDMA (Mailbox)
	dma_cb_t *cbp = ctl->cb;

	uint32_t phys_pwm_fifo_addr = 0x7e20c000 + 0x18;//PWM Fifo
	uint32_t phys_pwm_range_addr = 0x7e20c000 + 0x10;//PWM Range
	uint32_t phys_clock_div_addr = 0x7e101074;//CLOCK Frequency Setting
	uint32_t phys_pwm_clock_div_addr = 0x7e1010a4; //CLK PWM
	uint32_t phys_gpio_set_addr = 0x7e20001c;
	uint32_t phys_gpio_clear_addr = 0x7e200028;
	uint32_t dummy_gpio = 0x7e20b000;
	uint32_t phys_pcm_fifo_addr = 0x7e203004;
	uint32_t phys_gpio_pads_addr =0x7e10002c;		      
	uint32_t phys_pcm_clock =     0x7e10109c ;
	uint32_t phys_pcm_clock_div_addr = 0x7e10109c;//CLOCK Frequency Setting
	
	uint32_t phys_gpfsel = 0x7E200000 ; 
	int samplecnt;
	
		
	for (samplecnt = 0; samplecnt <  NUM_SAMPLES ; samplecnt++)
	{
		

//@0				
		//Set Amplitude by writing to PWM_SERIAL via PADS	
		cbp->info = 0;//BCM2708_DMA_NO_WIDE_BURSTS | BCM2708_DMA_WAIT_RESP  ;
		cbp->src = mem_virt_to_phys(&ctl->sample[samplecnt].Amplitude1);
		cbp->dst = phys_gpio_pads_addr;
		cbp->length = 4;
		cbp->stride = 0;
		cbp->next = mem_virt_to_phys(cbp + 1);		
		cbp++;
//@1				
		//Set Amplitude by writing to PWM_SERIAL via Patern	
		cbp->info = 0;//BCM2708_DMA_NO_WIDE_BURSTS | BCM2708_DMA_WAIT_RESP  ;
		cbp->src = mem_virt_to_phys(&ctl->sample[samplecnt].Amplitude2); 
		if(UsePCMClk==0) 
			cbp->dst = phys_pwm_fifo_addr;
		if(UsePCMClk==1) 
			cbp->dst = phys_gpfsel; 				
		cbp->length = 4;
		cbp->stride = 0;
		cbp->next = mem_virt_to_phys(cbp + 1); 
		cbp++;
//2
		//Set PWMFrequency
		cbp->info =/*BCM2708_DMA_NO_WIDE_BURSTS*/ BCM2708_DMA_SRC_INC|BCM2708_DMA_NO_WIDE_BURSTS;
		// BCM2708_DMA_WAIT_RESP : without 160ns, with 300ns
		cbp->src = mem_virt_to_phys(&ctl->sample[samplecnt].FrequencyTab[0]);
		if(UsePCMClk==0)		
			cbp->dst = phys_pwm_clock_div_addr;
		if(UsePCMClk==1)		
			cbp->dst = phys_clock_div_addr;
		cbp->length = 4; //Be updated by main DMA
		cbp->stride = 0;
		cbp->next = mem_virt_to_phys(cbp + 1); 
		cbp++;
	}
			
	cbp--;
	cbp->next = mem_virt_to_phys((void*)virtbase);
			

	// ------------------------------ END DMA INIT ---------------------------------
	
	dma_reg[DMA_CS+DMA_CHANNEL*0x40] = BCM2708_DMA_RESET;
	udelay(1000);
	dma_reg[DMA_CS+DMA_CHANNEL*0x40] = BCM2708_DMA_INT | BCM2708_DMA_END;
	udelay(100);
	dma_reg[DMA_CONBLK_AD+DMA_CHANNEL*0x40]=mem_virt_to_phys((void *)virtbase );
	udelay(100);
	dma_reg[DMA_DEBUG+DMA_CHANNEL*0x40] = 7; // clear debug error flags
	udelay(100);
	
	return 1;		
}

#define ln(x) (log(x)/log(2.718281828459045235f))

int arctan2(int y, int x) // Should be replaced with fast_atan2 from rtl_fm
{
	int abs_y = abs(y);
	int angle;
	if((x==0)&&(y==0)) return 0;
	if(x >= 0){
		angle = 45 - 45 * (x - abs_y) / ((x + abs_y)==0?1:(x + abs_y));
	} else {
		angle = 135 - 45 * (x + abs_y) / ((abs_y - x)==0?1:(abs_y - x));
	}
	return (y < 0) ? -angle : angle; // negate if in quad III or IV
}

void IQToFreqAmp(int I,int Q,double *Frequency,int *Amp,int SampleRate)
{
	double phase;
	static double prev_phase = 0;
	
	*Amp=round(sqrt( I*I + Q*Q)/sqrt(2));
	//*Amp=*Amp*3; // Amp*5 pour la voix !! To be tested more
	if(*Amp>32767)
	{
		printf("!");
		*Amp=32767; //Overload
	}

	phase = M_PI + ((float)arctan2(I,Q)) * M_PI/180.0f;
	double dp = phase - prev_phase;
	if(dp < 0) dp = dp + 2*M_PI;
    
	*Frequency = dp*SampleRate/(2.0f*M_PI);
	prev_phase = phase;
	//printf("I=%d Q=%d Amp=%d Freq=%d\n",I,Q,*Amp,*Frequency);
}




inline void shuffle_int(uint32_t list[], size_t len)
{		
	int j;						
	uint32_t tmp;					
	while(len)
	{					
		j = rand() % (len+1);				
		if (j != len - 1)
		{			
			tmp = list[j];			
			list[j] = list[len - 1];	
			list[len - 1] = tmp;		
		}					
		len--;					
	}						
}	

inline void FrequencyAmplitudeToRegister(double TuneFrequency,uint32_t Amplitude,int NoSample,uint32_t WaitNanoSecond,uint32_t SampleRate,char NoUsePWMF,int debug)
{
	static char ShowInfo=1;				
				
	static uint32_t CompteurDebug=0;
	#define DEBUG_RATE 20000
	int PwmNumberStep;
	CompteurDebug++;
	static uint32_t TabPwmAmplitude[18]={0x00000000,
									0x80000000,0xA0000000,0xA8000000,0xAA000000,
									0xAA800000,0xAAA00000,0xAAA80000,0xAAAA0000,
									0xAAAA8000,0xAAAAA000,0xAAAAA800,0xAAAAAA00,
									0xAAAAAA80,0xAAAAAAA0,0xAAAAAAA8,0xAAAAAAAA,0xAAAAAAAA};
	
	
	
	static char First=1;
				
	ctl = (struct control_data_s *)virtbase; // Struct ctl is mapped to the memory allocated by RpiDMA (Mailbox)
	dma_cb_t *cbp = ctl->cb+NoSample*CBS_SIZE_BY_SAMPLE;
				
	static dma_cb_t *cbpwrite;
	uint32_t TimeRemaining=0;
				
	int SkipFrequency=0; //When WaitNano is too short, we don't set the frequency

	// WITH DMA_CTL WITHOUT BCM2708_DMA_WAIT_RESP
	// Time = NBStep * 157 ns + 1360 ns
				
	
				
	
	if(WaitNanoSecond==0)
	{
		if(SampleRate!=0)
				WaitNanoSecond = (1e9/SampleRate);
	}	
				
	PwmNumberStep=WaitNanoSecond/FREQ_MINI_TIMING;
	if(PwmNumberStep>PWM_STEP_MAXI) PwmNumberStep=PWM_STEP_MAXI;
				

	// ********************************** PWM FREQUENCY PROCESSING *****************************
				
	if(UsePCMClk==0)
		TuneFrequency*=2.0; //Because of pattern 10
				
	// F1 < TuneFrequency < F2
	uint32_t FreqDividerf2=(int) ((double)PllUsed/TuneFrequency);
	uint32_t FreqFractionnalf2=4096.0 * (((double)PllUsed/TuneFrequency)-FreqDividerf2);
				
	uint32_t FreqDividerf1=(FreqFractionnalf2!=4095)?FreqDividerf2:FreqDividerf2+1;				
	uint32_t FreqFractionnalf1=(FreqFractionnalf2!=4095)?FreqFractionnalf2+1:0;
	
	double f1=PllUsed/(FreqDividerf1+(double)FreqFractionnalf1/4096.0);
	double f2=PllUsed/(FreqDividerf2+(double)FreqFractionnalf2/4096.0); // f2 is the higher frequency
	double FreqTuningUp=PllUsed/(FreqDividerf2+(double)FreqFractionnalf2/4096.0);

	double FreqStep=f2-f1;
	static uint32_t RegisterF1;
	static uint32_t RegisterF2;		
							
	if(ShowInfo==1)
	{
		printf("WaitNano=%d F1=%f TuneFrequency %f F2=%f Initial Resolution(Hz)=%f ResolutionPWMF %f NbStep=%d DELAYStep=%d\n",WaitNanoSecond,f1,TuneFrequency,f2,FreqStep,FreqStep/(PwmNumberStep),PwmNumberStep,(PWMF_MARGIN+FREQ_DELAY_TIME)/FREQ_MINI_TIMING);
		ShowInfo=0;
	}
				
	static int DebugStep=71;
	double	fPWMFrequency=((FreqTuningUp-TuneFrequency)*1.0*(double)(PwmNumberStep)/FreqStep); // Give NbStep of F2
	int PWMFrequency=round(fPWMFrequency);
				
	//printf("PWMF =%d PWMSTEP=%d\n",PWMFrequency,PwmNumberStep);
	/*if((CompteurDebug%DEBUG_RATE)==0)
	{
		DebugStep=(DebugStep+1)%PwmNumberStep;
		//printf("PwmNumberStep %d Step %d\n",PwmNumberStep,DebugStep);
	}
	PWMFrequency=(DebugStep);*/
				
	//if((CompteurDebug%200)==0) printf("PwmNumberStep =%d TuneFrequency %f : FreqTuning %f FreqStep %f PwmFreqStep %f fPWMFrequency %f PWMFrequency %d f1 %f f2 %f %x %x\n",PwmNumberStep,TuneFrequency,FreqTuning,FreqStep,FreqStep/PwmNumberStep,fPWMFrequency,PWMFrequency,f1,f2,RegisterF1,RegisterF2);

	int i;
				
	static int NbF1,NbF2,NbF1F2;
	NbF1=0;
	NbF2=0;
	NbF1F2=0;

	
	int AdaptPWMFrequency;			
	if((PwmNumberStep-PWMFrequency-(PWMF_MARGIN+FREQ_DELAY_TIME)/FREQ_MINI_TIMING)>PwmNumberStep/2)
	{
		RegisterF1=0x5A000000 | (FreqDividerf1<<12) | (FreqFractionnalf1);
		RegisterF2=0x5A000000 | (FreqDividerf2<<12) | (FreqFractionnalf2);
		AdaptPWMFrequency=PWMFrequency;
		NbF1=0;
		NbF2=(PWMF_MARGIN+FREQ_DELAY_TIME)/FREQ_MINI_TIMING;
		
	}
	else // SWAP F1 AND F2
	{
		//if((CompteurDebug%DEBUG_RATE)==0) printf("-");
		RegisterF2=0x5A000000 | (FreqDividerf1<<12) | (FreqFractionnalf1);
		RegisterF1=0x5A000000 | (FreqDividerf2<<12) | (FreqFractionnalf2);
		AdaptPWMFrequency=PwmNumberStep-PWMFrequency;
		NbF1=0;
		NbF2=(PWMF_MARGIN+FREQ_DELAY_TIME)/FREQ_MINI_TIMING;
	}
				
	i=0;
	NbF1F2=NbF1+NbF2;
	
	if(NoUsePWMF==1)
	{
		RegisterF1=0x5A000000 | (FreqDividerf1<<12) | (FreqFractionnalf1);
		RegisterF2=0x5A000000 | (FreqDividerf2<<12) | (FreqFractionnalf2);
		i=0;
		ctl->sample[NoSample].FrequencyTab[i++]=RegisterF2;
	}
	else
	{			
		while(NbF1F2<PwmNumberStep-1)
		{
			if(NbF1<AdaptPWMFrequency) 
			{
				ctl->sample[NoSample].FrequencyTab[i++]=RegisterF1;
				NbF1++;
				NbF1F2++;
			}
			if(NbF2<PwmNumberStep-AdaptPWMFrequency-1)
			{
				ctl->sample[NoSample].FrequencyTab[i++]=RegisterF2;
				NbF2++;
				NbF1F2++;
			}
		}
		if (Randomize)
			shuffle_int(ctl->sample[NoSample].FrequencyTab,i);
			
		//SHould finished by F2
		ctl->sample[NoSample].FrequencyTab[i++]=RegisterF2;
		NbF2++;
		NbF1F2++;
	}	
				
	cbpwrite=cbp+2;
	cbpwrite->length=i*4;

	
				
	// ****************************** AMPLITUDE PROCESSING **********************************************
				
	
	Amplitude=(Amplitude>32767)?32767:Amplitude;	
	int IntAmplitude=(Amplitude*7.0)/32767.0; // Convert to 8 amplitude step
				
	if(UsePCMClk==0)
	{
		if(IntAmplitude==0)
		{
			ctl->sample[NoSample].Amplitude2=0x0;
		}
		else
		{
			ctl->sample[NoSample].Amplitude2=0xAAAAAAAA;					
		}
	}
	if(UsePCMClk==1)
	{
		if(IntAmplitude==0)
		{
			ctl->sample[NoSample].Amplitude2=(Originfsel & ~(7 << 12)) | (0 << 12);
		}
		else
		{
			ctl->sample[NoSample].Amplitude2=(Originfsel & ~(7 << 12)) | (4 << 12);
		}
	}

	static int OldIntAmplitude=0;
				
	
	if(IntAmplitude>7) IntAmplitude=7;
	
	ctl->sample[NoSample].Amplitude1=0x5a000000 + (IntAmplitude&0x7) + (1<<4) + (0<<3); 
				

}





int GetDMADelay(int Step)
{
	
	//Calibrate DMA Rate
	// =====================================================
	static volatile uint32_t cur_cb,last_cb;
	struct timespec gettime_now;
	int32_t start_time,time_difference;
	int last_sample;
	int this_sample; 
	int free_slots;

	dma_cb_t *cbp = ctl->cb;
	cur_cb = (uint32_t)virtbase; // DMA AT 1st CBS
	dma_reg[DMA_CONBLK_AD+DMA_CHANNEL*0x40]=mem_virt_to_phys((void*)cur_cb);
	usleep(100);
	int samplecnt;
	for (samplecnt = 0; samplecnt <  NUM_SAMPLES ; samplecnt++)
	{
		
		cbp+=2;
		cbp->length = (uint32_t)4L*Step;
		cbp++;
	}
	
	dma_reg[DMA_CS+DMA_CHANNEL*0x40] = DMA_CS_PRIORITY(7) | DMA_CS_PANIC_PRIORITY(7) | DMA_CS_DISDEBUG |DMA_CS_ACTIVE;	// START DMA : go, mid priority, wait for outstanding writes :7 Seems Max Priority
	usleep(5000); //Wait to be sure DMA is running stable
	int i;
	int SumDelay=0;
	for(i=0;i<10;i++)
	{

	

		last_cb = mem_phys_to_virt((uint32_t)(dma_reg[DMA_CONBLK_AD+DMA_CHANNEL*0x40]));
		last_sample = (last_cb - (uint32_t)virtbase) / (sizeof(dma_cb_t) * CBS_SIZE_BY_SAMPLE);

		clock_gettime(CLOCK_REALTIME, &gettime_now);
		start_time = gettime_now.tv_nsec;

		// ONE SAMPLE SHOULD BE MINIMUM AROUND Time = NBStep * 157 ns + 1360 ns +1360*4 ns = 6us and MAX 200*157+1360*5 = 35us
		// Sleep at 80% de DELAY*NUM_SAMPLE
		// BY removing 2 CBP = 300ns gain
		do
		{
			usleep(10);
			cur_cb = mem_phys_to_virt((uint32_t)(dma_reg[DMA_CONBLK_AD+DMA_CHANNEL*0x40]));
			this_sample = (cur_cb - (uint32_t)virtbase) / (sizeof(dma_cb_t) * CBS_SIZE_BY_SAMPLE);
			free_slots = this_sample - last_sample;
			if (free_slots < 0) // WARNING : ORIGINAL CODE WAS < strictly
				free_slots += NUM_SAMPLES;
			clock_gettime(CLOCK_REALTIME, &gettime_now);
		
		}
		while(free_slots<=NUM_SAMPLES*0.6);
	 	
	

		time_difference = gettime_now.tv_nsec - start_time;
		if(time_difference<0) time_difference+=1E9;

		//printf("Delay = %d\n",time_difference/free_slots);

		SumDelay+=time_difference/free_slots;
	}
	
	//STop DMA
	dma_reg[DMA_CS+DMA_CHANNEL*0x40] |= DMA_CS_ABORT;//BCM2708_DMA_INT | BCM2708_DMA_END;
	udelay(100);
	dma_reg[DMA_CS+DMA_CHANNEL*0x40]&= ~DMA_CS_ACTIVE;
	dma_reg[DMA_CS+DMA_CHANNEL*0x40] |= DMA_CS_RESET; //BCM2708_DMA_ABORT|BCM2708_DMA_RESET;
	udelay(100);

	return SumDelay/10;
}

int CalibrateSystem(int *ppm,int *BaseDelayDMA,int *StepDelayDMA)
{
	struct timex ntx;
	int status;
	//Calibrate Clock system (surely depends also on PLL PPM
	// =====================================================

	ntx.modes = 0; /* only read */
  	status = ntp_adjtime(&ntx);
	double clockppm;

  	if (status != TIME_OK)
	{
    		printf("Error: NTP\n");
		return 0;
 	}
	clockppm = (double)ntx.freq/(double)(1 << 16);
	if(abs(clockppm)<200)
		*ppm=clockppm;
	//printf("Clock PPM = %f\n",ppm);
	int i; 
	int BaseDelay=0;
	BaseDelay=GetDMADelay(0);

	*BaseDelayDMA=BaseDelay;
	*StepDelayDMA=(GetDMADelay(PWM_STEP_MAXI/2)-(*BaseDelayDMA))/(PWM_STEP_MAXI/2);
	//for(i=1;i<200;i+=10)
		//printf("Step %d =%d\n",i,(GetDMADelay(i)-BaseDelay)/i);
	return 1;
}

int pitx_init(int SampleRate, double TuningFrequency, int* skipSignals,int SetDma)
{	
	InitGpio();
	InitDma(terminate, skipSignals);
	if(SetDma) DMA_CHANNEL=SetDma;
	SetupGpioClock(SampleRate,TuningFrequency);
//int FREQ_MINI_TIMING=157;
//int PWMF_MARGIN = 1120; //A Margin for now at 1us with PCM ->OK

	if(CalibrateSystem(&globalppmpll,&PWMF_MARGIN,&FREQ_MINI_TIMING))
		printf("Calibrate : ppm=%d DMA %dns:%dns\n",globalppmpll,FREQ_MINI_TIMING,PWMF_MARGIN);
	//printf("Timing : 1 cyle=%dns 1sample=%dns\n",NBSAMPLES_PWM_FREQ_MAX*400*3,(int)(1e9/(float)SampleRate));
	return 1;
}

void print_usage(void)
{

fprintf(stderr,\
"\nrpitx -%s\n\
Usage:\nrpitx [-i File Input][-m ModeInput] [-f frequency output] [-s Samplerate] [-l] [-p ppm] [-h] \n\
-m            {IQ(FileInput is a Stereo Wav contains I on left Channel, Q on right channel)}\n\
              {IQFLOAT(FileInput is a Raw float interlaced I,Q)}\n\
              {RF(FileInput is a (double)Frequency,Time in nanoseconds}\n\
       	      {RFA(FileInput is a (double)Frequency,(int)Time in nanoseconds,(float)Amplitude}\n\
	      {VFO (constant frequency)}\n\
-i            path to File Input \n\
-f float      frequency to output on GPIO_18 pin 12 in khz : (130 kHz to 750 MHz),\n\
-l            loop mode for file input\n\
-p float      frequency correction in parts per million (ppm), positive or negative, for calibration, default 0.\n\
-d int 	      DMABurstSize (default 1000) but for very short message, could be decrease\n\
-h            help (this help).\n\
\n",\
PROGRAM_VERSION);

} /* end function print_usage */



double GlobalTuningFrequency;
int HarmonicNumber =1;

int pitx_SetTuneFrequency(double Frequency)
{
	#define MAX_HARMONIC 41
	int harmonic;
	printf("Setting tuning frequency, input %f Hz\n",Frequency);
	
	if(Frequency<PLL_FREQ_1GHZ/2048L) //2/4096-> For very Low Frequency we used 19.2 MHZ PLL 
	{
		PllUsed=PllFreq19MHZ;
		PllNumber=PLL_192;
	}
	else 
	{
		PllUsed=PllFreq1GHZ;
		PllNumber=PLL_1GHZ;
	}
	
	printf("Master PLL = %d\n",PllUsed);
		
	for(harmonic=1;harmonic<MAX_HARMONIC;harmonic+=2)
	{
		printf("->%f harmonic %d\n",(Frequency/(double)harmonic),harmonic);
		if((Frequency/(double)harmonic)<=(double)PllUsed/4.0) break;
	}
	HarmonicNumber=harmonic;	

	//HarmonicNumber=11; //TEST

	if(HarmonicNumber>1) //Use Harmonic
	{
		GlobalTuningFrequency=Frequency/HarmonicNumber;			
		printf("\n Warning : Using harmonic %d\n",HarmonicNumber);
	}
	else
	{
		GlobalTuningFrequency=Frequency;
	}
	return 1;
}

/** Wrapper around read. */
static ssize_t readFile(void *buffer, const size_t count) 
{
	return read(FileInHandle, buffer, count);
}
static void resetFile(void) 
{
	lseek(FileInHandle, 0, SEEK_SET);
}

int main(int argc, char* argv[])
{
	int a;
	int anyargs = 0;
	char Mode = MODE_IQ;  // By default
	int SampleRate=48000;
	float SetFrequency=1e6;//1MHZ
	float ppmpll=0.0;
	char NoUsePwmFrequency=0;
	int SetDma=0;
	while(1)
	{
		a = getopt(argc, argv, "i:f:m:s:p:hld:w:c:ra:");
	
		if(a == -1) 
		{
			if(anyargs) break;
			else a='h'; //print usage and exit
		}
		anyargs = 1;	

		switch(a)
		{
		case 'i': // File name
			FileName = optarg;
			break;
		case 'f': // Frequency
			SetFrequency = atof(optarg);
			printf ("SetFreq: %e kHz\n",SetFrequency);
			break;
		case 'm': // Mode (IQ,IQFLOAT,RF,RFA)
			if(strcmp("IQ",optarg)==0) Mode=MODE_IQ;
			if(strcmp("RF",optarg)==0) Mode=MODE_RF;	
			if(strcmp("RFA",optarg)==0) Mode=MODE_RFA;
			if(strcmp("IQFLOAT",optarg)==0) Mode=MODE_IQ_FLOAT;
			if(strcmp("VFO",optarg)==0) Mode=MODE_VFO;
			break;
		case 's': // SampleRate (Only needeed in IQ mode)
			SampleRate = atoi(optarg);
			break;
		case 'p':  //	ppmcorrection
			ppmpll = atof(optarg);
			
			break;
		case 'h': // help
			print_usage();
			exit(1);
			break;
		case 'l': // loop mode
			loop_mode_flag = 1;
			break;
		case 'd': // Dma Sample Burst
			DmaSampleBurstSize = atoi(optarg);
			NUM_SAMPLES=4*DmaSampleBurstSize;
			break;
		case 'c': // Use clock instead of PWM pin
			UsePCMClk = atoi(optarg);
			if(UsePCMClk==1) printf("Use GPCLK Pin instead of PWM\n");
			break;
		case 'w': // No use pwmfrequency 
			NoUsePwmFrequency = atoi(optarg);
			
			break;
		case 'r': // Randomize PWM frequency 
			Randomize=1;
			
			break;
		case 'a': // DMA Channel 1-14
			 if((atoi(optarg)>0)&&(atoi(optarg)<15))
			 {
				SetDma=atoi(optarg);
				//DMA_CHANNEL=SetDma; Should be set after initdma
			 }
			 else
				SetDma=0;
			break;
        	case -1:
        	break;
		case '?':
			if (isprint(optopt) )
 			{
 				fprintf(stderr, "rpitx: unknown option `-%c'.\n", optopt);
 			}
			else
			{
				fprintf(stderr, "rpitx: unknown option character `\\x%x'.\n", optopt);
			}
			print_usage();

			exit(1);
			break;			
		default:
			print_usage();
			exit(1);
			break;
		}/* end switch a */
	}/* end while getopt() */

	//Open File Input for modes which need it
	if((Mode==MODE_IQ)||(Mode==MODE_IQ_FLOAT)||(Mode==MODE_RF)||(Mode==MODE_RFA))
	{
		if(FileName && strcmp(FileName,"-")==0)
		{
			FileInHandle = STDIN_FILENO;
			useStdin = 1;
		}
		else FileInHandle = open(FileName, O_RDONLY);
		if (FileInHandle < 0)
		{
			fatal("Failed to read Filein %s\n",FileName);
		}
	}

	resetFile();
	return pitx_run(Mode, SampleRate, SetFrequency, ppmpll, NoUsePwmFrequency, readFile, resetFile, NULL,SetDma);
}

int pitx_run(
	const char Mode,
	int SampleRate,
	const float SetFrequency,
	float ppmpll,
	const char NoUsePwmFrequency,
	ssize_t (*readWrapper)(void *buffer, size_t count),
	void (*reset)(void),
	int* skipSignals,
	int SetDma) 
{
	int i;
	//char pagemap_fn[64];

	int OffsetModulation=1000;//TBR
	int MicGain=100;
	//unsigned char *data;

	//Specific to ModeIQ
	static signed short *IQArray=NULL;

	//Specific to ModeIQ_FLOAT
	static float *IQFloatArray=NULL;

	//Specific to Mode RF
	typedef struct {
		double Frequency;
		uint32_t WaitForThisSample;
	} samplerf_t;
	samplerf_t *TabRfSample=NULL;

	fprintf(stdout,"rpitx Version %s compiled %s (F5OEO Evariste) running on ",PROGRAM_VERSION,__DATE__);

	// Init Plls Frequency using ppm (or default)
	if(ppmpll!=0) ppmpll=(float)globalppmpll; // Use calibrate only if not setting by user
	PllFreq500MHZ=PLL_FREQ_500MHZ;
	PllFreq500MHZ+=PllFreq500MHZ * (ppmpll / 1000000.0);

	PllFreq1GHZ=PLL_FREQ_1GHZ;
	PllFreq1GHZ+=PllFreq1GHZ * (ppmpll / 1000000.0);

	PllFreq19MHZ=PLLFREQ_192;
	PllFreq19MHZ+=PllFreq19MHZ * (ppmpll / 1000000.0);

	//End of Init Plls

	if(Mode==MODE_IQ)
	{
		IQArray=malloc(DmaSampleBurstSize*2*sizeof(signed short)); // TODO A FREE AT THE END OF SOFTWARE
		reset();
	} 
	if(Mode==MODE_IQ_FLOAT)
	{
		IQFloatArray=malloc(DmaSampleBurstSize*2*sizeof(float)); // TODO A FREE AT THE END OF SOFTWARE
	}
	if((Mode==MODE_RF)||(Mode==MODE_RFA))
	{
		//TabRfSample=malloc(DmaSampleBurstSize*sizeof(samplerf_t));
		SampleRate=50000L; //NOT USED BUT BY CALCULATING TIMETOSLEEP IN RF MODE
	}
	if(Mode==MODE_VFO)
		SampleRate=50000L; //50000 BY EXPERIMENT

	if(Mode==MODE_IQ)
	{
		printf(" Frequency=%f ",GlobalTuningFrequency);
		printf(" SampleRate=%d ",SampleRate);	
	}


	

	pitx_SetTuneFrequency(SetFrequency*1000.0);
	pitx_init(SampleRate, GlobalTuningFrequency, skipSignals,SetDma);
	

	static volatile uint32_t cur_cb,last_cb;
	int last_sample;
	int this_sample; 
	int free_slots;
	//int SumDelay=0;
	
	long int start_time;
	static long time_difference=0;
	struct timespec gettime_now;

	cur_cb = (uint32_t)virtbase+ (NUM_SAMPLES-DmaSampleBurstSize)* sizeof(dma_cb_t) *CBS_SIZE_BY_SAMPLE;
	
	last_cb=(uint32_t)virtbase /*+ 965* sizeof(dma_cb_t) *CBS_SIZE_BY_SAMPLE*/ ;


	dma_reg[DMA_CONBLK_AD+DMA_CHANNEL*0x40]=mem_virt_to_phys((void*)cur_cb);

	unsigned char Init=1;

// -----------------------------------------------------------------

	for (;;) 
	{
		int TimeToSleep;
		static int StatusCompteur=0;
			
		cur_cb = mem_phys_to_virt((uint32_t)(dma_reg[DMA_CONBLK_AD+DMA_CHANNEL*0x40]));
		this_sample = (cur_cb - (uint32_t)virtbase) / (sizeof(dma_cb_t) * CBS_SIZE_BY_SAMPLE);
		last_sample = (last_cb - (uint32_t)virtbase) / (sizeof(dma_cb_t) * CBS_SIZE_BY_SAMPLE);
		free_slots = this_sample - last_sample;
		if (free_slots < 0) // WARNING : ORIGINAL CODE WAS < strictly
			free_slots += NUM_SAMPLES;
				
		//printf("last_sample %lx cur_cb %lx FreeSlots = %d Time to sleep=%d\n",last_sample,cur_cb,free_slots,TimeToSleep);
			
		if(Init==0)
		{
			if((Mode==MODE_RF)||(Mode==MODE_RFA))
			{
				TimeToSleep=100; //Max 100KHZ
			}
			else
				TimeToSleep=(1e6*(NUM_SAMPLES-free_slots*2))/SampleRate; // Time to sleep in us
			//printf("TimeToSleep%d\n",TimeToSleep);
		}
		else
			TimeToSleep=1000;
			
		//printf("Buffer Available=%d\n",BufferAvailable());
			
		clock_gettime(CLOCK_REALTIME, &gettime_now);
		start_time = gettime_now.tv_nsec;		
		if(TimeToSleep>=(2200+KERNEL_GRANULARITY)) // 2ms : Time to process File/Canal Coding
		{
			udelay(TimeToSleep-(2200+KERNEL_GRANULARITY));
			TimeToSleep=0;
		}
		else
		{
			//udelay(TimeToSleep);
			sched_yield();
			//TimeToSleep=0;
			//if(free_slots>(NUM_SAMPLES*9/10))
			//printf("Buffer nearly empty...%d/%d\n",free_slots,NUM_SAMPLES);
		}
			
		static int free_slots_now;
		cur_cb = mem_phys_to_virt(dma_reg[DMA_CONBLK_AD+DMA_CHANNEL*0x40]);
		this_sample = (cur_cb - (uint32_t)virtbase) / (sizeof(dma_cb_t) * CBS_SIZE_BY_SAMPLE);
		last_sample = (last_cb - (uint32_t)virtbase) / (sizeof(dma_cb_t) * CBS_SIZE_BY_SAMPLE);
		free_slots_now = this_sample - last_sample;
		if (free_slots_now < 0) // WARNING : ORIGINAL CODE WAS < strictly
			free_slots_now += NUM_SAMPLES;
			
		clock_gettime(CLOCK_REALTIME, &gettime_now);
		time_difference = gettime_now.tv_nsec - start_time;
		if(time_difference<0) time_difference+=1E9;
			
		if(StatusCompteur%10==0)
		{ 
			//printf(" DiffTime = %ld FreeSlot %d FreeSlotDiff=%d Bitrate : %f\n",time_difference,free_slots_now,free_slots_now-free_slots,(1e9*(free_slots_now-free_slots))/(float)time_difference);	
		}
		//if((1e9*(free_slots_now-free_slots))/(float)time_difference<40100.0) printf("Drift BAD\n"); else printf("Drift GOOD\n");	
		StatusCompteur++;
		free_slots=free_slots_now;
		// FIX IT : Max(freeslot et Numsample/8)
		if((Init==1)&&(free_slots < DmaSampleBurstSize /*NUM_SAMPLES/8*/))
		{
			printf("****** STARTING TRANSMIT ********\n");
			dma_reg[DMA_CONBLK_AD+DMA_CHANNEL*0x40]=mem_virt_to_phys((void*)virtbase );
			usleep(100);
			//Start DMA PWMFrequency
				
			//dma_reg[DMA_CS+DMA_CHANNEL_PWMFREQUENCY*0x40] = 0x10880001;				

			//Start Main DMA
			dma_reg[DMA_CS+DMA_CHANNEL*0x40] = DMA_CS_PRIORITY(7) | DMA_CS_PANIC_PRIORITY(7) | DMA_CS_DISDEBUG |DMA_CS_ACTIVE;
			
				
			Init=0;
				
			continue;
		}
		clock_gettime(CLOCK_REALTIME, &gettime_now);
		start_time = gettime_now.tv_nsec;
			
		int debug=0;

		if ((free_slots>=DmaSampleBurstSize)) 
		{
		// *************************************** MODE IQ **************************************************
			if(Mode==MODE_IQ)
			{
				int NbRead=0;
				static int Max=0;
				static int Min=32767;
				static int CompteSample=0;
				CompteSample++;
				NbRead=readWrapper(IQArray,DmaSampleBurstSize*2*2/*SHORT I,SHORT Q*/);
				
				if(NbRead!=DmaSampleBurstSize*2*2) 
				{
					if(loop_mode_flag==1)
					{
						printf("Looping FileIn\n");
						reset();
						NbRead=readWrapper(IQArray,DmaSampleBurstSize*2*2);
					}
					else {
						stop_dma();
						return 0;
					}
				}
				
				for(i=0;i<DmaSampleBurstSize;i++)
				{
					//static float samplerate=48000;
					static int amp;
					static double df;
					
					int CorrectionRpiFrequency=-1000; //TODO PPM / Offset=1KHZ at 144MHZ
					
					CompteSample++;
					//printf("i%d q%d\n",IQArray[2*i],IQArray[2*i+1]);
					
					IQToFreqAmp(IQArray[2*i+1],IQArray[2*i],&df,&amp,SampleRate);
						
					// Compression have to be done in modulation (SSB not here)
						
					double A = 87.7f; // compression parameter
					double ampf=amp/32767.0;
      				ampf = (fabs(ampf) < 1.0f/A) ? A*fabs(ampf)/(1.0f+ln(A)) : (1.0f+ln(A*fabs(ampf)))/(1.0f+ln(A)); //compand
					amp= (int)(round(ampf * 32767.0f)) ;
						
					if(amp>Max) Max=amp;
					if(amp<Min) Min=amp;	
						
					/*
					if((CompteSample%4800)==0)
					{
						//printf("%d\n",((CompteSample/48000)*1024)%32767);
						//printf("Amp %d Freq %f MinAmp %d MaxAmp %d\n",amp,df,Min,Max);
						printf("%d;%d;%d;%f\n",IQArray[2*i+1],IQArray[2*i],amp,df);
						// printf(".");
						fflush(stdout);
					}
					*/
					// TEST - WARNING, REMOVE FOR RELEASE 
					//amp=((CompteSample/48000)*1024)%32767;
					//df=0;
					//amp=amp*10;
					//amp=32767;
					//if(df<OffsetModulation+100) amp=0;
					//if(amp<32767/8) df= OffsetModulation;
					//

					// FIXME : df/harmonicNumber could alterate maybe modulations
					FrequencyAmplitudeToRegister((GlobalTuningFrequency-OffsetModulation+/*(CompteSample/480)*/+df/HarmonicNumber)/HarmonicNumber,amp,last_sample++,0,SampleRate,NoUsePwmFrequency,CompteSample%2);
					// !!!!!!!!!!!!!!!!!!!! 680 is for 48KHZ , should be adpated !!!!!!!!!!!!!!!!!

					free_slots--;
					if (last_sample == NUM_SAMPLES)	last_sample = 0;
				}
			}
		// *************************************** MODE IQ FLOAT**************************************************
			if(Mode==MODE_IQ_FLOAT)
			{
				int NbRead=0;
				static int Max=0;
				static int Min=32767;
				static int CompteSample=0;
				CompteSample++;
				NbRead=readWrapper(IQFloatArray,DmaSampleBurstSize*2*sizeof(float));
				
				if(NbRead!=DmaSampleBurstSize*2*sizeof(float)) 
				{
					if(loop_mode_flag==1)
					{
						printf("Looping FileIn\n");
						reset();
					}
					else if (!useStdin) {
						stop_dma();
						return 0;
					}
				}
				
				for(i=0;i<DmaSampleBurstSize;i++)
				{
					//static float samplerate=48000;
					static int amp;
					static double df;
					
					int CorrectionRpiFrequency=-1000; //TODO PPM / Offset=1KHZ at 144MHZ
					
					CompteSample++;
					//printf("i%d q%d\n",IQArray[2*i],IQArray[2*i+1]);
					
					IQToFreqAmp(IQFloatArray[2*i+1]*32767,IQFloatArray[2*i]*32767,&df,&amp,SampleRate);

					if(amp>Max) Max=amp;
					if(amp<Min) Min=amp;
					/*	
					if((CompteSample%4800)==0)
					{
						//printf("%d\n",((CompteSample/48000)*1024)%32767);
						printf("Amp %d Freq %f MinAmp %d MaxAmp %d\n",amp,df,Min,Max);
						// printf(".");
						fflush(stdout);
					}
					*/
					// TEST - WARNING, REMOVE FOR RELEASE 
					//amp=((CompteSample/48000)*1024)%32767;
					//df=0;
					//amp=amp*10;
					//amp=32767;
					//if(df<OffsetModulation+100) amp=0;
					//
					//amp=32767;
					//if(df>SampleRate/2) df=SampleRate/2-df;
					FrequencyAmplitudeToRegister((GlobalTuningFrequency-OffsetModulation+df/HarmonicNumber)/HarmonicNumber,amp,last_sample++,0,SampleRate,NoUsePwmFrequency,CompteSample%2);
						
					free_slots--;
					if (last_sample == NUM_SAMPLES)	last_sample = 0;
				}
			}
		// *************************************** MODE RF **************************************************
			if((Mode==MODE_RF)||(Mode==MODE_RFA))
			{
				// SHOULD NOT EXEED 200 STEP*500ns; SAMPLERATE SHOULD BE MAX TO HAVE PRECISION FOR PCM 
				// BUT FIFO OF PCM IS 16 : SAMPLERATE MAYBE NOT EXCESS 16*80000 ! CAREFULL BUGS HERE
				#define MAX_DELAY_WAIT (PWM_STEP_MAXI/2*FREQ_MINI_TIMING-PWMF_MARGIN) 
				static int CompteSample=0;
				static uint32_t TimeRemaining=0;
				static samplerf_t SampleRf;
				static int NbRead;
				CompteSample++;
				int i;
				for(i=0;i<DmaSampleBurstSize;i++)
				{
					if(TimeRemaining==0)
					{
						NbRead=readWrapper(&SampleRf,sizeof(samplerf_t));
						if(NbRead!=sizeof(samplerf_t)) 
						{
							if(loop_mode_flag==1)
							{
								//printf("Looping FileIn\n");
								reset();
								NbRead=readWrapper(&SampleRf,sizeof(samplerf_t));
							}
							else if (!useStdin)
							{
								stop_dma();
								return 0;
							}
						}
							
						TimeRemaining=SampleRf.WaitForThisSample;
						//TimeRemaining=50000;//SampleRf.WaitForThisSample;
						debug=1;
						//printf("A=%f Time =%d \n",SampleRf.Frequency,SampleRf.WaitForThisSample);
					}
					else
						debug=0;	
					
					static int amp=32767;
					static int WaitSample=0;
					
					if(TimeRemaining>MAX_DELAY_WAIT) 
						WaitSample=MAX_DELAY_WAIT;	
					else
						WaitSample=TimeRemaining;
					
					//printf("TimeRemaining %d WaitSample %d\n",TimeRemaining,WaitSample);
					if(Mode==MODE_RF)
					{
						if(SampleRf.Frequency==0.0)
						{
							amp=0;
							SampleRf.Frequency=00.0;// TODO change that ugly frequency
						}
						else
							amp=32767;
						FrequencyAmplitudeToRegister((SampleRf.Frequency/HarmonicNumber+GlobalTuningFrequency)/HarmonicNumber,amp,last_sample++,WaitSample,0,NoUsePwmFrequency,debug);
					}
					if(Mode==MODE_RFA)
						FrequencyAmplitudeToRegister((GlobalTuningFrequency)/HarmonicNumber,SampleRf.Frequency,last_sample++,WaitSample,0,NoUsePwmFrequency,debug);

					TimeRemaining-=WaitSample;
					free_slots--;
					if (last_sample == NUM_SAMPLES)	last_sample = 0;
				}
			}
				
		// *************************************** MODE VFO **************************************************
			if(Mode==MODE_VFO)
			{
				static uint32_t CompteSample=0;
					
				int i;
				//printf("Begin free %d\n",free_slots);
				for(i=0;i<DmaSampleBurstSize;i++)
				{						
					//To be fine tuned !!!!	
					static int OutputPower=32767;
					CompteSample++;
					debug=1;//(debug+1)%2;	
					//OutputPower=(CompteSample/10)%32768;

					FrequencyAmplitudeToRegister(GlobalTuningFrequency/HarmonicNumber/*+(CompteSample*0.1)*/,OutputPower,last_sample++,25000,0,NoUsePwmFrequency,debug);
					free_slots--;
					//printf("%f \n",GlobalTuningFrequency+(((CompteSample/10)*1)%50000));	
					if (last_sample == NUM_SAMPLES)	last_sample = 0;
					if(CompteSample%40000==0)
					{
						//OutputPower=(OutputPower+1)%8;
						//pad_gpios_reg[PADS_GPIO_0] = 0x5a000000 + (OutputPower&0x7) + (1<<4) + (0<<3); // Set output power for I/Q GPIO18/GPIO19 
						//printf("Freq %d Outputpower=%d\n",CompteSample/20000,OutputPower);
					}
					//usleep(1);
				}
					//printf("End free %d\n",free_slots);
			}

		}
			
		clock_gettime(CLOCK_REALTIME, &gettime_now);
		time_difference = gettime_now.tv_nsec - start_time;
		if(time_difference<0) time_difference+=1E9;
			
		last_cb = (uint32_t)virtbase + last_sample * sizeof(dma_cb_t) * CBS_SIZE_BY_SAMPLE;
	}
				
	stop_dma();
	return(0);
}

