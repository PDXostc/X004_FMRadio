#
/*
 *    Copyright (C) 2014
 *    Jan van Katwijk (J.vanKatwijk@gmail.com)
 *    Lazy Chair Programming
 *
 *    This file is part of the  SDR-J series.
 *    Many of the ideas as implemented in the SDR-J are derived from
 *    other work, made available through the (a) GNU general Public License. 
 *    All copyrights of the original authors are recognized.
 *
 *    SDR-J is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    SDR-J is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include	<Qt>
#include	<QSettings>
#include	<QMessageBox>
#include	<QDebug>
#include	<QDateTime>
#include	<QFileDialog>
#include	<QDir>
#include	"fm-constants.h"
#include	"gui.h"
#include	"fm-processor.h"
#include	"fm-demodulator.h"
#include	"rds-decoder.h"
#include	"audiosink.h"
#include	"virtual-input.h"
#include	"dabstick-dll.h"
#include	"sdrplay.h"
#include	"mirics-dongle.h"
#include	"popup-keypad.h"

#ifdef __MINGW32__
#include	<iostream>
#include	<windows.h>
#endif
//
//	Processing modes
#define	IDLE		0100
#define	PAUSED		0101
#define	RUNNING		0102
#define	STOPPING	0103
//
//
static
int16_t	delayTable [] = {1, 3, 5, 7, 9, 10, 15};
#define delayTableSize	((int)(sizeof (delayTable) / sizeof (int16_t)))
/*
 *	We use the creation function merely to set up the
 *	user interface and make the connections between the
 *	gui elements and the handling agents. All real action
 *	is embedded in actions, initiated by gui buttons
 */
	RadioInterface::RadioInterface (QSettings	*Si,
	                                const char	*deviceName,
	                                QWidget		*parent): QDialog (parent) {
int16_t	i;
QString h;
int	k;
bool	success;
int32_t	startFreq;

	setupUi (this);
	fmSettings		= Si;
	runMode			= IDLE;
	squelchMode		= false;
//
//	not used - other than as dummy parameters - in the "mini"
	this		-> audioRate	=
	                     fmSettings	-> value ("audioRate",
	                                           22050). toInt ();
	if (audioRate == 22050) {
	   this -> inputRate = 48 * audioRate;
	   this -> fmRate    = 8 * audioRate;
	}
	else
	if (audioRate == 44100) {
	   this -> inputRate = 24 * audioRate;
	   this -> fmRate    = 4  * audioRate;
	}
	else {
	   audioRate = 48000;
	   fmRate    = 192000;
	   inputRate = 960000;
	}

	if (deviceName != NULL)
	   fmDevice	= QString (deviceName);
	else
	   fmDevice	= fmSettings -> value ("device", "dongle"). toString ();

	if ((fmDevice == "dabstick") || (fmDevice == "dabStick"))
	   myRig = new dabstick_dll (inputRate, &success);
	else
	if ((fmDevice == "sdrPlay") || (fmDevice == "sdrplay"))
	   myRig = new sdrplay (inputRate, &success);
	else {
	   fprintf (stderr, "Trying to load the dongle\n");
	   myRig = new miricsDongle (inputRate, &success);
	}
	
	if (!success) {
	   QMessageBox::warning (this, tr ("sdr"),
	                               tr ("Device problem\n"));
	   exit (1);
	}
	
	startFreq		= myRig	-> defaultFrequency	();
	startFreq		= fmSettings -> value ("frequency",
	                                                startFreq). toInt ();
	setTuner (startFreq);
	Display (myRig -> getVFOFrequency ());
	int16_t	temp =
		fmSettings -> value ("the_gain", 13). toInt ();
	gainSelector		-> setMaximum (myRig -> maxGain ());
	gainSelector		-> setValue (temp);
	setGainSelector	(gainSelector -> value ());

	myFMprocessor		= NULL;
	our_audioSink		= new audioSink (this -> audioRate);
	outTable		= new int16_t
	                             [our_audioSink -> numberofDevices () + 1];
	for (i = 0; i < our_audioSink -> numberofDevices (); i ++)
	   outTable [i] = -1;

	if (!setupSoundOut (streamOutSelector,
	                    our_audioSink,
	                    this -> audioRate,
	                    outTable)) {
	   fprintf (stderr, "Cannot open any output device\n");
	   abortSystem (33);
	}

	h	= fmSettings -> value ("streamOutSelector",
	                                    "default"). toString ();
	k	= streamOutSelector -> findText (h);
	if (k != -1) {
	   streamOutSelector -> setCurrentIndex (k);
	   setStreamOutSelector (k);
	}
	
//
	audioDumping		= false;
	audiofilePointer	= NULL;
	audioDump		-> setText ("audioDump");

//	Set relevant sliders etc to the value they had last time
	restoreGUIsettings	(fmSettings);

	incrementingFlag ->
	                 setStyleSheet ("QLabel {background-color:blue}");
	incrementingFlag -> setText (" ");
	IncrementIndex		= 0;
//	settings for the auto tuner
	IncrementIndex		=
	             fmSettings -> value ("IncrementIndex", 0). toInt ();
	fmIncrement		=
	             fmSettings -> value ("fm_increment", 100). toInt ();

	minLoopFrequency	=
	   fmSettings -> value ("min_loop_frequency", 86500). toInt ();
	if (minLoopFrequency == 0)
	   minLoopFrequency = 86500;

	maxLoopFrequency	=
	   fmSettings -> value ("max_loop_frequency", 110000). toInt ();
	if (maxLoopFrequency == 0)
	   maxLoopFrequency = 110000;

	fm_increment	-> setValue (fmIncrement);	// 
	minimumSelect	-> setValue (KHz (minLoopFrequency) / MHz (1));
	maximumSelect	-> setValue (KHz (maxLoopFrequency) / MHz (1));

//	he does the connections from the gui buttons, sliders etc
	localConnects		();
//
//	Create a timer for autoincrement/decrement of the tuning
	autoIncrementTimer	= new QTimer ();
	autoIncrementTimer	-> setSingleShot (true);
	autoIncrementTimer	-> setInterval (5000);
	connect (autoIncrementTimer,
	         SIGNAL (timeout ()),
	         this,
	         SLOT (autoIncrement_timeout ()));
//
//	create a timer for displaying the "real" time
	displayTimer		= new QTimer ();
	displayTimer		-> setInterval (1000);
	connect (displayTimer,
	         SIGNAL (timeout ()),
	         this,
	         SLOT (updateTimeDisplay ()));
//
//
//	Display the version
	QString v = "sdrJ-FM -V";
	v. append (CURRENT_VERSION);
	systemindicator	-> setText (v. toLatin1 (). data ());

	pauseButton		-> setText (QString ("Pause"));
	currentPIcode		= 0;
	frequencyforPICode	= 0;
	int16_t	thresHold	= fmSettings -> value ("threshold", 20).
	                                               toInt ();
//
//	The FM processor is currently shared with the
//	regular FM software, so lots on dummy parameters
	myFMprocessor	= new fmProcessor  (myRig,
	                                    this,
	                                    our_audioSink,
	                                    inputRate,
	                                    fmRate,
	                                    this -> audioRate,
	                                    thresHold);
	lcd_fmRate		-> display ((int)this -> fmRate);
	lcd_inputRate		-> display ((int)this -> inputRate);
	lcd_OutputRate		-> display ((int)this -> audioRate);
	connect (fm_increment, SIGNAL (valueChanged (int)),
	              this, SLOT (set_fm_increment (int)));
	connect (minimumSelect, SIGNAL (valueChanged (int)),
	              this, SLOT (set_minimum (int)));
	connect (maximumSelect, SIGNAL (valueChanged (int)),
	              this, SLOT (set_maximum (int)));
//
	setAttenuation		(attenuationSlider	-> value ());
	setVolume		(volumeSlider		-> value ());
	setfmMode		(fmMode			-> currentText ());
	setfmRdsSelector	(fmRdsSelector		-> currentText ());
	setfmDecoder		(fmDecoder		-> currentText ());
	setfmChannelSelector	(fmChannelSelect	-> currentText ());
	setfmDeemphasis		(fmDeemphasisSelector	-> currentText ());
	myFMprocessor		-> set_squelchMode (squelchMode);
	set_squelchValue	(squelchSlider		-> value ());

	displayTimer		-> start (1000);
	mykeyPad		= new keyPad (this);
	connect (freqButton, SIGNAL (clicked (void)),
	         this, SLOT (handle_freqButton (void)));
}

	RadioInterface::~RadioInterface () {
	delete		autoIncrementTimer;
	delete		displayTimer;
	delete		our_audioSink;
	if (myFMprocessor != NULL)
	   delete myFMprocessor;
	if (mykeyPad -> isVisible ())
	   mykeyPad -> hidePad ();
	delete mykeyPad;
}
//
//	When pressing the freqButton, a form will appear on
//	which the "user" may key in a frequency. 
//
//	If it is already there, pressing the button (for the second time)
//	will hide the keypad again
void	RadioInterface::handle_freqButton (void) {
	if (mykeyPad -> isVisible ())
	   mykeyPad -> hidePad ();
	else
	   mykeyPad	-> showPad ();
}

void	RadioInterface::newFrequency (int f) {
	stopIncrementing	();
	setTuner (f);
	Display (myRig -> getVFOFrequency ());
}
//
//	When stopping, we save the values of some state elements
void	RadioInterface::dumpControlState	(QSettings *s) {

	if (s == NULL)		// should not happen
	   return;

	s	-> setValue ("the_gain", gainSelector -> value ());
	s	-> setValue ("device", fmDevice);
	s	-> setValue ("frequency", myRig -> getVFOFrequency ());
	s	-> setValue ("fm_increment",
	                               fm_increment -> value ());
	s	-> setValue ("attenuationSlider",
	                               attenuationSlider -> value ());
	s	-> setValue ("VolumeSlider",
	                               volumeSlider	-> value ());
//
//	now setting the parameters for the fm decoder
	s	-> setValue ("fmMode",
	                               fmMode 		-> currentText ());
	s	-> setValue ("fmDecoder",
	                               fmDecoder	-> currentText ());
	s	-> setValue ("fmRdsSelector",
	                               fmRdsSelector	-> currentText ());
	s	-> setValue ("fmChannelSelect",
	                               fmChannelSelect	-> currentText ());
	s	-> setValue ("fmDeemphasisSelector",
	                               fmDeemphasisSelector -> currentText ());
	s	-> setValue ("streamOutSelector",
	                               streamOutSelector -> currentText ());

	s	-> setValue ("min_loop_frequency",
	                               minLoopFrequency);
	s	-> setValue ("max_loop_frequency",
	                               maxLoopFrequency);
}
//
//	On start, we ensure that the streams are stopped so
//	that they can be restarted again.
void	RadioInterface::setStart	(void) {
bool	r = 0;

	if (runMode == RUNNING)
	   return;
//
//	always ensure that datastreams are stopped
	myRig		-> stopReader ();
	our_audioSink 	-> stop ();
//
	r = myRig		-> restartReader ();
	qDebug ("Starting %d\n", r);
	if (!r) {
	   QMessageBox::warning (this, tr ("sdr"),
	                               tr ("Opening  input stream failed\n"));
	   return;
	}

	our_audioSink	-> restart ();

//	and finally: recall that starting overrules pausing
	pauseButton -> setText (QString ("Pause"));
	runMode	= RUNNING;
}

void	RadioInterface::TerminateProcess (void) {
	runMode		= STOPPING;
	stopIncrementing	();
	if (audioDumping) {
	   our_audioSink	-> stopDumping ();
	   sf_close	(audiofilePointer);
	}
//
//	It is pretty important that no one is attempting to
//	set things within the FMprocessor when it is
//	being deleted. Correct order is
//	stop the Reader first
	myRig		-> stopReader ();
	usleep (100);
	myFMprocessor	-> stop ();
	
	accept ();
	if (fmSettings != NULL)
	   dumpControlState	(fmSettings);
	qDebug () <<  "Termination started";
}

void	RadioInterface::abortSystem (int d) {
	qDebug ("aborting for reason %d\n", d);
	accept ();
}
//
//
void	RadioInterface::setGainSelector (int g) {
	myRig	-> setExternalGain (g);
}

void	RadioInterface::setfmChannelSelector (const QString &s) {
uint8_t	channelSelector;

	if (s == "stereo")
	   channelSelector	= fmProcessor::S_STEREO;
	else
	if (s == "Left")
	   channelSelector	= fmProcessor::S_LEFT;
	else
	if (s == "Right")
	   channelSelector	= fmProcessor::S_RIGHT;
	else
	if (s == "Left+Right")
	   channelSelector	= fmProcessor::S_LEFTplusRIGHT;
	else
	if (s == "Left-Right")
	   channelSelector	= fmProcessor::S_LEFTminusRIGHT;
	else		// the default
	   channelSelector	= fmProcessor::S_LEFT;
	if (myFMprocessor != NULL)
	   myFMprocessor	-> setSoundMode	(channelSelector);
}

void	RadioInterface::setAttenuation (int n) {
	attenuationLevelDisplay	-> display (n);
	if (myFMprocessor != NULL)
	   myFMprocessor	-> setAttenuation (2 * n);
}
//	Increment frequency: with amount N, depending
//	on the mode of operation
//
int32_t 	RadioInterface::mapIncrement (int32_t n) {
	return n * 1000;
}
//
//	The generic setTuner.
void	RadioInterface::setTuner (int32_t n) {
	myRig		-> setVFOFrequency		(n);
	if (myFMprocessor != NULL)
	   myFMprocessor	-> resetRds	();
}

void	RadioInterface::set_plusOne	(void) {
	myRig	-> setVFOFrequency (myRig -> getVFOFrequency () + KHz (1));
}

void	RadioInterface::set_minusOne	(void) {
	myRig	-> setVFOFrequency (myRig -> getVFOFrequency () - KHz (1));
}

//
//===== code for auto increment/decrement
//	lots of code for something simple,

static inline
bool	frequencyInBounds (int32_t f, int32_t l, int32_t u) {
	return l <= f && f <= u;
}

int32_t	RadioInterface::IncrementInterval (int16_t index) {
	if (index < 0)
	   index = - index;

	if (index == 0)
	   index = 1;
	if (index >= delayTableSize)
	   index = delayTableSize;

	return 1000 * delayTable [index - 1];
}

void	RadioInterface::set_incrementFlag (int16_t incr) {
char temp [128];

	if (incr == 0) {
	   incrementingFlag ->
	                 setStyleSheet ("QLabel {background-color:blue}");
	   incrementingFlag -> setText (" ");
	   return;
	}
	if (incr < 0)
	   sprintf (temp, " << %d", IncrementInterval (incr) / 1000);
	else 
	   sprintf (temp, "%d >> ", IncrementInterval (incr) / 1000);
	incrementingFlag ->
	                 setStyleSheet ("QLabel {background-color:green}");
	incrementingFlag -> setText (temp);
}
//
void	RadioInterface::autoIncrement_timeout (void) {
int32_t	amount;
int32_t	frequency;
int32_t	low, high;

	low	= KHz (minLoopFrequency);
	high	= KHz (maxLoopFrequency);
	amount	=  fmIncrement;
	if (IncrementIndex < 0)
	   amount = - amount;
//
	frequency	= myRig -> getVFOFrequency () + KHz (amount);

	if ((IncrementIndex < 0) &&
	   !frequencyInBounds (frequency, low, high))
	   frequency = high;

	if ((IncrementIndex > 0) &&
	   !frequencyInBounds (frequency, low, high))
	   frequency = low;

	setTuner (frequency);
	Display (myRig -> getVFOFrequency ());
	autoIncrementTimer	-> start (IncrementInterval (IncrementIndex));
	myFMprocessor	-> startScanning ();
}

void	RadioInterface::scanresult	(void) {
	stopIncrementing ();
}
//
//	stopIncrementing is called from various places to
//	just interrupt the autoincrementing
void	RadioInterface::stopIncrementing (void) {
	set_incrementFlag (0);

	if (autoIncrementTimer	-> isActive ())
	   autoIncrementTimer -> stop ();

	IncrementIndex = 0;
	myFMprocessor	-> stopScanning ();
}

void	RadioInterface::autoIncrementButton (void) {

	if (autoIncrementTimer	-> isActive ())
	   autoIncrementTimer -> stop ();

	if (++IncrementIndex > delayTableSize)
	   IncrementIndex = delayTableSize;

	if (IncrementIndex == 0) {
	   set_incrementFlag (0);
	   return;
	}
//
	autoIncrementTimer	-> start (IncrementInterval (IncrementIndex));
	set_incrementFlag (IncrementIndex);
}

void	RadioInterface::autoDecrementButton (void) {
	if (autoIncrementTimer	-> isActive ())
	   autoIncrementTimer -> stop ();

	if (--IncrementIndex < - delayTableSize)
	   IncrementIndex = - delayTableSize;

	if (IncrementIndex == 0) {
	   set_incrementFlag (0);
	   return;
	}
//
	autoIncrementTimer	-> start (IncrementInterval (IncrementIndex));
	set_incrementFlag (IncrementIndex);
}

void	RadioInterface::set_fm_increment (int v) {
	fmIncrement	= v;		// in Khz
}
//
//	min and max frequencies are specified in Mhz
void	RadioInterface::set_minimum	(int f) {
	   minLoopFrequency	= Khz (f);
}

void	RadioInterface::set_maximum	(int f) {
	   maxLoopFrequency	= Khz (f);
}

void	RadioInterface::IncrementButton (void) {
	stopIncrementing ();
	setTuner (myRig -> getVFOFrequency () + Khz (fmIncrement));
	Display (myRig -> getVFOFrequency ());
}

void	RadioInterface::DecrementButton (void) {
	stopIncrementing ();
	setTuner (myRig -> getVFOFrequency () - Khz (fmIncrement));
	Display (myRig -> getVFOFrequency ());
}
//

void	RadioInterface::updateTimeDisplay (void) {
QDateTime	currentTime = QDateTime::currentDateTime ();

	timeDisplay	-> setText (currentTime.
	                            toString (QString ("dd.MM.yy:hh:mm:ss")));
	showStrength ();
}

/*
 *	there is a tremendous amount of signal/slot connections
 *	The local connects, knobs, sliders and displays,
 *	are connected here.
 */
void	RadioInterface::localConnects (void) {
	connect (startButton, SIGNAL (clicked ()),
	              this, SLOT (setStart ()));
	connect	(pauseButton, SIGNAL (clicked (void)),
	               this, SLOT (clickPause (void)));
	connect (quitButton, SIGNAL (clicked ()),
	              this, SLOT (TerminateProcess (void)));
	connect (streamOutSelector, SIGNAL (activated (int)),
	              this, SLOT (setStreamOutSelector (int)));
	connect	(volumeSlider, SIGNAL (valueChanged (int)),
	              this, SLOT (setVolume (int)));
	connect (plusOne, SIGNAL (clicked (void)),
	         this, SLOT (set_plusOne (void)));
	connect (minusOne, SIGNAL (clicked (void)),
	         this, SLOT (set_minusOne (void)));

	connect (audioDump, SIGNAL (clicked (void)),
	              this, SLOT (set_audioDump (void)));
	connect (squelchButton , SIGNAL (clicked (void)),
	              this, SLOT (set_squelchMode (void)));
	connect (squelchSlider, SIGNAL (valueChanged (int)),
	              this, SLOT (set_squelchValue (int)));

	connect (attenuationSlider, SIGNAL (valueChanged (int) ),
		      this, SLOT (setAttenuation (int) ) );
	connect (gainSelector, SIGNAL (valueChanged (int)),
	              this, SLOT (setGainSelector (int)));

	connect (fc_plus, SIGNAL (clicked (void)),
	              this, SLOT (autoIncrementButton (void)));
	connect (fc_minus, SIGNAL (clicked (void)),
	              this, SLOT (autoDecrementButton (void)));
	connect (f_plus, SIGNAL (clicked (void)),
	              this, SLOT (IncrementButton (void)));
	connect (f_minus, SIGNAL (clicked (void)),
	              this, SLOT (DecrementButton (void)));
//
//	fm specific buttons and sliders
	connect (fmChannelSelect, SIGNAL (activated (const QString &)),
	               this, SLOT (setfmChannelSelector (const QString &)));
	connect (fmMode, SIGNAL (activated (const QString &)),
	               this, SLOT (setfmMode (const QString &)));
	connect (fmRdsSelector, SIGNAL (activated (const QString &)),
	               this, SLOT (setfmRdsSelector (const QString &)));
	connect (fmDecoder, SIGNAL (activated (const QString &)),
	               this, SLOT (setfmDecoder (const QString &)));
	connect (fmDeemphasisSelector, SIGNAL (activated (const QString &)),
	              this, SLOT (setfmDeemphasis (const QString &)));
}

//	Deemphasis	= 50 usec (3183 Hz, Europe)
//	Deemphasis	= 75 usec (2122 Hz US)
void	RadioInterface::setfmDeemphasis	(const QString& s) {
	if (myFMprocessor == NULL)
	   return;
	if (s == "50")
	   myFMprocessor	-> setDeemphasis (50);
	else
	if (s == "75")
	   myFMprocessor	-> setDeemphasis (75);
	else
	   myFMprocessor	-> setDeemphasis (1);
}

void	RadioInterface::setVolume (int v) {
	if (myFMprocessor != NULL)
	   myFMprocessor	-> setVolume ((int16_t)v);
}
//

void	RadioInterface::setCRCErrors	(int n) {
//	crcErrors	-> display (n);
	(void)n;
}

void	RadioInterface::setSyncErrors	(int n) {
//	syncErrors	-> display (n);
	(void)n;
}

void	RadioInterface::setbitErrorRate	(double v) {
//	bitErrorRate	-> display (v);
	(void)v;
}

void	RadioInterface::setGroup	(int n) {
	(void)n;
//	rdsGroupDisplay	-> display (n);
}

void	RadioInterface::setPTYCode	(int n) {
	(void)n;
//	rdsPTYDisplay	-> display (n);
}

void	RadioInterface::setAFDisplay	(int n) {
	rdsAFDisplay	-> display (n);
}

void	RadioInterface::setPiCode	(int n) {
int32_t	t	= myRig -> getVFOFrequency ();

	if ((frequencyforPICode != t) || (n != 0)) {
	   currentPIcode	= n;
	   frequencyforPICode = t;
	}

	rdsPiDisplay	-> display (n);
}

void	RadioInterface::clearStationLabel (void) {
	StationLabel = QString ("");
	stationLabelTextBox	-> setText (StationLabel);
}
//
//	Note: although s is a char * type, its value does not
//	end with a zero, so it is not a C string
void	RadioInterface::setStationLabel (char *s, int size) {
uint16_t	i;

	StationLabel = QString ("");
	for (i = 0; i < size; i ++)
	   StationLabel. append (QString (QChar (s [i])));
	stationLabelTextBox	-> setText (StationLabel);
}

void	RadioInterface::setMusicSpeechFlag (int n) {
	if (n != 0)
	   speechLabel -> setText (QString ("music"));
	else
	   speechLabel -> setText (QString ("speech"));
}

void	RadioInterface::clearMusicSpeechFlag (void) {
	speechLabel	-> setText (QString (""));
}

void	RadioInterface::clearRadioText (void) {
	RadioText = QString ("");
	radioTextBox	-> setText (RadioText);
}
//
//	Note that, although s is a "char *", it is not a C string,
//	(no zero at the end)
void	RadioInterface::setRadioText (char *s, int size) {
uint16_t	i;
	RadioText	= QString ("");
	for (i = 0; i < size; i ++) {
	   RadioText. append (QString (QChar (s [i])));
	}
	radioTextBox	-> setText (RadioText);
}

void	RadioInterface::setRDSisSynchronized (bool syn) {
	if (!syn)
	   rdsSyncLabel -> setStyleSheet ("QLabel {background-color:red}");
	else
	   rdsSyncLabel -> setStyleSheet ("QLabel {background-color:green}");
}
//

void	RadioInterface::setfmMode (const QString &s) {
	myFMprocessor	-> setfmMode (s == "stereo");
}

void	RadioInterface::setfmRdsSelector (const QString &s) {
	rdsModus = (s == "rds 1" ? rdsDecoder::RDS1 :
	            s == "rds 2" ? rdsDecoder::RDS2 : 
	            rdsDecoder::NO_RDS);
	myFMprocessor	-> setfmRdsSelector (rdsModus);
}

void	RadioInterface::setfmDecoder (const QString &s) {
int8_t	decoder	= 0;

	if (s == "fm1decoder")
	   decoder = fm_Demodulator::FM1DECODER;
	else
	if (s == "fm2decoder")
	   decoder = fm_Demodulator::FM2DECODER;
	else
	if (s == "fm3decoder")
	   decoder = fm_Demodulator::FM3DECODER;
	else
	if (s == "fm4decoder")
	   decoder = fm_Demodulator::FM4DECODER;
	else
	   decoder = fm_Demodulator::FM5DECODER;

	myFMprocessor	-> setFMdecoder (decoder);
	decoderDisplay	-> setText (QString (myFMprocessor -> nameofDecoder ()));
}
//
void	RadioInterface::Display (int32_t freq) {
	lcd_Frequency	-> setDigitCount (6);
	lcd_Frequency	-> display ((int)freq / KHz (1));
}
//
//	This signal will arrive once every "inputRate" samples
void	RadioInterface::showStrength (void) {
bool locked		= myFMprocessor -> isLocked ();
float the_dcComponent	= myFMprocessor -> get_dcComponent ();
	if (locked)
	   pll_isLocked -> setStyleSheet ("QLabel {background-color:green}");
	else
	   pll_isLocked -> setStyleSheet ("QLabel {background-color:red}");
	dc_component	-> display (the_dcComponent);
}
//
void	RadioInterface::clickPause (void) {
	if (runMode == IDLE)
	   return;

	if (runMode == RUNNING) {
	   if (autoIncrementTimer	-> isActive ())
	      autoIncrementTimer 	-> stop ();

	   myRig		-> stopReader	();
	   our_audioSink	-> stop		();
	   pauseButton -> setText (QString ("Continue"));
	   runMode = PAUSED;
	}
	else
	if (runMode == PAUSED) {
	   if (IncrementIndex != 0)	// restart the incrementtimer if needed
	      autoIncrementTimer	-> start (IncrementInterval (IncrementIndex));
	   myRig		-> restartReader ();
	   our_audioSink	-> restart ();
	   pauseButton -> setText (QString ("Pause"));
	   runMode = RUNNING;
	}
}

//
//
//	do not forget that ocnt starts with 1, due
//	to Qt list conventions
bool	RadioInterface::setupSoundOut (QComboBox	*streamOutSelector,
	                               audioSink	*our_audioSink,
	                               int32_t		cardRate,
	                               int16_t		*table) {
uint16_t	ocnt	= 1;
uint16_t	i;

	for (i = 0; i < our_audioSink -> numberofDevices (); i ++) {
	   const char *so =
	             our_audioSink -> outputChannelwithRate (i, cardRate);
	   qDebug ("Investigating Device %d\n", i);

	   if (so != NULL) {
	      streamOutSelector -> insertItem (ocnt, so, QVariant (i));
	      table [ocnt] = i;
	      qDebug (" (output):item %d wordt stream %d\n", ocnt , i);
	      ocnt ++;
	   }
	}

	qDebug () << "added items to combobox";
	return ocnt > 1;
}

void	RadioInterface::setStreamOutSelector (int idx) {
	if (idx == 0)
	   return;

	outputDevice = outTable [idx];
	if (!our_audioSink -> isValidDevice (outputDevice)) 
	   return;

	our_audioSink	-> stop	();
	if (!our_audioSink -> selectDevice (outputDevice)) {
	   QMessageBox::warning (this, tr ("sdr"),
	                               tr ("Selecting  output stream failed\n"));
	   return;
	}

	qWarning () << "selected output device " << idx << outputDevice;
	our_audioSink	-> restart ();
}

////////////////////////////////////////////////////////////////////

void	RadioInterface::set_squelchMode	(void) {
	if (myFMprocessor == NULL)
	   return;
	squelchMode = !squelchMode;
	squelchButton	-> setText (squelchMode ? QString ("squelchOn") :
	                                          QString ("squelchOff"));
	myFMprocessor -> set_squelchMode (squelchMode);
}

void	RadioInterface::set_squelchValue (int n) {
	if (myFMprocessor != NULL)
	   myFMprocessor -> set_squelchValue (n);
}
//
//	Just a simple routine to set the sliders and boxes
//	to their previous values
void	RadioInterface::restoreGUIsettings (QSettings *s) {
QString h;
int	k;

	k	= s -> value ("attenuationSlider", 50). toInt ();
	attenuationSlider	-> setValue (k);
	k	= s -> value ("VolumeSlider", 10). toInt ();
	volumeSlider		-> setValue (k);

	k	= s -> value ("squelchSlider",
	                          squelchSlider -> value ()). toInt ();
	squelchSlider		-> setValue (k);

	h	= s -> value ("fmMode",
	                          fmMode -> currentText ()). toString ();
	k	= fmMode -> findText (h);
	if (k != -1)
	   fmMode -> setCurrentIndex (k);

	h	= s -> value ("fmRdsSelector",
	                          fmRdsSelector -> currentText ()). toString ();
	k	= fmRdsSelector -> findText (h);
	if (k != -1)
	   fmRdsSelector -> setCurrentIndex (k);

	h	= s -> value ("fmDecoder",
	                          fmDecoder -> currentText ()). toString ();
	k	= fmDecoder -> findText (h);
	if (k != -1)
	   fmDecoder -> setCurrentIndex (k);

	h	= s -> value ("fmChannelSelect",
	                          fmChannelSelect -> currentText ()). toString ();
	k	= fmChannelSelect	-> findText (h);
	if (k != -1)
	   fmChannelSelect -> setCurrentIndex (k);

	h	= s -> value ("fmDeemphasisSelector",
	                          fmDeemphasisSelector -> currentText ()). toInt ();
	k	= fmDeemphasisSelector -> findText (h);
	if (k!= -1)
	   fmDeemphasisSelector -> setCurrentIndex (k);
}


void	RadioInterface::set_audioDump (void) {
SF_INFO	*sf_info	= (SF_INFO *)alloca (sizeof (SF_INFO));

	if (audioDumping) {
	   our_audioSink	-> stopDumping ();
	   sf_close (audiofilePointer);
	   audioDumping = false;
	   audioDump	-> setText ("audioDump");
	   return;
	}

	QString file = QFileDialog::getSaveFileName (this,
	                                        tr ("open file .."),
	                                        QDir::homePath (),
	                                        tr ("Sound (*.wav)"));
	file		= QDir::toNativeSeparators (file);
	sf_info		-> samplerate	= this -> audioRate;
	sf_info		-> channels	= 2;
	sf_info		-> format	= SF_FORMAT_WAV | SF_FORMAT_PCM_24;

	audiofilePointer	= sf_open (file. toLatin1 (). data (),
	                                   SFM_WRITE, sf_info);
	if (audiofilePointer == NULL) {
	   qDebug () << "Cannot open " << file. toLatin1 (). data ();
	   return;
	}

	audioDump		-> setText ("WRITING");
	audioDumping		= true;
	our_audioSink		-> startDumping (audiofilePointer);
}

