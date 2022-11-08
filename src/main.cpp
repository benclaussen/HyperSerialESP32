/* main.cpp
*
*  MIT License
*
*  Copyright (c) 2022 awawa-dev
*
*  https://github.com/awawa-dev/HyperSerialESP32
*
*  Permission is hereby granted, free of charge, to any person obtaining a copy
*  of this software and associated documentation files (the "Software"), to deal
*  in the Software without restriction, including without limitation the rights
*  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
*  copies of the Software, and to permit persons to whom the Software is
*  furnished to do so, subject to the following conditions:
*
*  The above copyright notice and this permission notice shall be included in all
*  copies or substantial portions of the Software.

*  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
*  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
*  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
*  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
*  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
*  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
*  SOFTWARE.
 */

#include <Arduino.h>
#include <NeoPixelBus.h>
#ifdef NEOPIXEL_RGBW
	#include "calibration.h"
	void printCalibration();
#endif

#if ESP_ARDUINO_VERSION_MAJOR >= 2
    #error "Please use esp32 Arduino version 1.x. Versions 2.x and above are unsupported."
#endif

// DO NOT EDIT THIS FILE. ADJUST THE CONFIGURATION IN THE platformio.ini
#define _STR(x) #x
#define _XSTR(x) _STR(x)
#define VAR_NAME_VALUE(var) #var " = " _XSTR(var)

#ifdef NEOPIXEL_RGBW
	#pragma message(VAR_NAME_VALUE(NEOPIXEL_RGBW))
#endif
#ifdef NEOPIXEL_RGB
	#pragma message(VAR_NAME_VALUE(NEOPIXEL_RGB))
#endif
#ifdef COLD_WHITE
	#pragma message(VAR_NAME_VALUE(COLD_WHITE))
#endif
#ifdef SPILED_APA102
	#pragma message(VAR_NAME_VALUE(SPILED_APA102))
#endif
#ifdef SPILED_WS2801
	#pragma message(VAR_NAME_VALUE(SPILED_WS2801))
#endif
#pragma message(VAR_NAME_VALUE(SERIALCOM_SPEED))

#ifdef NEOPIXEL_RGBW
	#define LED_DRIVER NeoPixelBus<NeoGrbwFeature, NeoEsp32I2s1800KbpsMethod>
#elif NEOPIXEL_RGB
	#define LED_DRIVER NeoPixelBus<NeoGrbFeature, NeoEsp32I2s1800KbpsMethod>
#elif SPILED_APA102
	#define LED_DRIVER NeoPixelBus<DotStarBgrFeature, DotStarEsp32DmaHspiMethod>
#elif SPILED_WS2801
	#define LED_DRIVER NeoPixelBus<NeoRbgFeature, NeoWs2801Spi2MhzMethod>
#endif

// STATS (sent only when there is no communication)
struct
{
	unsigned long start = 0;
	uint16_t goodFrames = 0;
	uint16_t totalFrames = 0;
	uint16_t finalGoodFrames = 0;
	uint16_t finalTotalFrames = 0;

	void update(unsigned long curTime)
	{
		if (totalFrames > 0 && totalFrames >= goodFrames)
		{
			finalGoodFrames = goodFrames;
			finalTotalFrames = totalFrames;
		}

		start = curTime;
		goodFrames = 0;
		totalFrames = 0;
	}

	void print(unsigned long curTime)
	{
		start = curTime;
		goodFrames = 0;
		totalFrames = 0;
		
		Serial.write("\r\nLast HyperHDR stats. Frames: ");
		Serial.print(finalTotalFrames);
		Serial.write(", good: ");
		Serial.print(finalGoodFrames);
		Serial.write("(FPS), incompl.: ");
		Serial.print(finalTotalFrames - finalGoodFrames);

		#if defined(NEOPIXEL_RGBW)
			printCalibration();
		#endif

	}
} stats;

#define MAX_BUFFER 4096
struct
{
	// LED strip number
	int ledsNumber = 0;
	// Makuna NeoPixelBusLibrary object
	LED_DRIVER *ledStrip = NULL;
	// want to render a frame?
	bool wantShow = false;
	// static data buffer for the loop
	uint8_t buffer[MAX_BUFFER];

	void initLedStrip(int count)
	{
		if (ledStrip != NULL)
			delete ledStrip;

		ledsNumber = count;
		#if defined(NEOPIXEL_RGBW) || defined(NEOPIXEL_RGB)
			ledStrip = new LED_DRIVER(ledsNumber, DATA_PIN);
			ledStrip->Begin();
		#else
			ledStrip = new LED_DRIVER(ledsNumber);
			ledStrip->Begin(CLOCK_PIN, 12, DATA_PIN, 15);
		#endif
		
	}

	inline void renderLeds()
	{
		if (wantShow && ledStrip != NULL && ledStrip->CanShow())
		{
			stats.goodFrames++;
			wantShow = false;
			ledStrip->Show();
		}
	}
} base;

// CALIBRATION & COLORSPACE RELATED STUFF
#ifdef NEOPIXEL_RGBW

	RgbwColor inputColor;

	void printCalibration()
	{
		Serial.write("\r\nRGBW => Gain: ");
		Serial.print(calibrationConfig.gain);
		Serial.write("/255, red: ");
		Serial.print(calibrationConfig.red);
		Serial.write(" , green: ");
		Serial.print(calibrationConfig.green);
		Serial.write(" , blue: ");
		Serial.print(calibrationConfig.blue);
	}

	inline void setStripPixel(uint16_t pix, RgbwColor &inputColor)
	{
		if (pix < base.ledsNumber)
		{
			base.ledStrip->SetPixelColor(pix, inputColor);
		}
	}

#else

	RgbColor inputColor;

	inline void setStripPixel(uint16_t pix, RgbColor &inputColor)
	{
		if (pix < base.ledsNumber)
		{
			base.ledStrip->SetPixelColor(pix, inputColor);
		}
	}

#endif

// PROTOCOL DEFINITION
enum class AwaProtocol
{
	HEADER_A,
	HEADER_w,
	HEADER_a,
	HEADER_HI,
	HEADER_LO,
	HEADER_CRC,
	VERSION2_GAIN,
	VERSION2_RED,
	VERSION2_GREEN,
	VERSION2_BLUE,
	RED,
	GREEN,
	BLUE,
	FLETCHER1,
	FLETCHER2
};

// AWA FRAME PROPERTIES
struct
{
	AwaProtocol state = AwaProtocol::HEADER_A;
	bool protocolVersion2 = false;
	uint8_t CRC = 0;
	uint16_t count = 0;
	uint16_t currentLed = 0;
	uint16_t fletcher1 = 0;
	uint16_t fletcher2 = 0;

	inline void init(byte input)
	{
		currentLed = 0;
		count = input * 0x100;
		CRC = input;
		fletcher1 = 0;
		fletcher2 = 0;
	}

	inline void addFletcher(byte input)
	{
		fletcher1 = (fletcher1 + (uint16_t)input) % 255;
		fletcher2 = (fletcher2 + fletcher1) % 255;
	}
} frameState;

// INCOMING CALIBRATION DATA
struct
{
	uint8_t gain = 0;
	uint8_t red = 0;
	uint8_t green = 0;
	uint8_t blue = 0;
} incoming;

void readSerialData()
{
	unsigned long curTime = millis();
	uint16_t bufferPointer = 0;
	uint16_t internalIndex = min(Serial.available(), MAX_BUFFER);

	if (internalIndex > 0)
		internalIndex = Serial.readBytes(base.buffer, internalIndex);

	if (internalIndex > 0 && curTime - stats.start > 1000)
	{
		stats.update(curTime);
	}
	else if (curTime - stats.start > 5000)
	{
		stats.print(curTime);
	}

	if (frameState.state == AwaProtocol::HEADER_A)
		base.renderLeds();

	while (bufferPointer < internalIndex)
	{
		byte input = base.buffer[bufferPointer++];
		switch (frameState.state)
		{
		case AwaProtocol::HEADER_A:
			frameState.protocolVersion2 = false;
			if (input == 'A')
				frameState.state = AwaProtocol::HEADER_w;
			break;

		case AwaProtocol::HEADER_w:
			if (input == 'w')
				frameState.state = AwaProtocol::HEADER_a;
			else
				frameState.state = AwaProtocol::HEADER_A;
			break;

		case AwaProtocol::HEADER_a:
			if (input == 'a')
				frameState.state = AwaProtocol::HEADER_HI;
			else if (input == 'A')
			{
				frameState.state = AwaProtocol::HEADER_HI;
				frameState.protocolVersion2 = true;
			}
			else
				frameState.state = AwaProtocol::HEADER_A;
			break;

		case AwaProtocol::HEADER_HI:
			stats.totalFrames++;
			frameState.init(input);
			frameState.state = AwaProtocol::HEADER_LO;
			break;

		case AwaProtocol::HEADER_LO:
			frameState.count += input;
			frameState.CRC = frameState.CRC ^ input ^ 0x55;
			frameState.state = AwaProtocol::HEADER_CRC;
			break;

		case AwaProtocol::HEADER_CRC:
			if (frameState.CRC == input)
			{
				if (frameState.count + 1 != base.ledsNumber)
					base.initLedStrip(frameState.count + 1);

				frameState.state = AwaProtocol::RED;
			}
			else
				frameState.state = AwaProtocol::HEADER_A;
			break;

		case AwaProtocol::RED:
			inputColor.R = input;
			frameState.addFletcher(input);

			frameState.state = AwaProtocol::GREEN;
			break;

		case AwaProtocol::GREEN:
			inputColor.G = input;
			frameState.addFletcher(input);

			frameState.state = AwaProtocol::BLUE;
			break;

		case AwaProtocol::BLUE:
			inputColor.B = input;
			frameState.addFletcher(input);

			#ifdef NEOPIXEL_RGBW
				inputColor.W = min(channelCorrection.red[inputColor.R],
								min(channelCorrection.green[inputColor.G],
									channelCorrection.blue[inputColor.B]));
				inputColor.R -= channelCorrection.red[inputColor.W];
				inputColor.G -= channelCorrection.green[inputColor.W];
				inputColor.B -= channelCorrection.blue[inputColor.W];
				inputColor.W = channelCorrection.white[inputColor.W];
			#endif

			setStripPixel(frameState.currentLed++, inputColor);

			if (frameState.count-- > 0)
				frameState.state = AwaProtocol::RED;
			else
			{
				if (frameState.protocolVersion2)
					frameState.state = AwaProtocol::VERSION2_GAIN;
				else
					frameState.state = AwaProtocol::FLETCHER1;
			}

			break;

		case AwaProtocol::VERSION2_GAIN:
			incoming.gain = input;
			frameState.addFletcher(input);

			frameState.state = AwaProtocol::VERSION2_RED;
			break;

		case AwaProtocol::VERSION2_RED:
			incoming.red = input;
			frameState.addFletcher(input);

			frameState.state = AwaProtocol::VERSION2_GREEN;
			break;

		case AwaProtocol::VERSION2_GREEN:
			incoming.green = input;
			frameState.addFletcher(input);

			frameState.state = AwaProtocol::VERSION2_BLUE;
			break;

		case AwaProtocol::VERSION2_BLUE:
			incoming.blue = input;
			frameState.addFletcher(input);

			frameState.state = AwaProtocol::FLETCHER1;
			break;

		case AwaProtocol::FLETCHER1:
			if (input != frameState.fletcher1)
				frameState.state = AwaProtocol::HEADER_A;
			else
				frameState.state = AwaProtocol::FLETCHER2;
			break;

		case AwaProtocol::FLETCHER2:
			if (input == frameState.fletcher2)
			{
				base.wantShow = true;
				base.renderLeds();

				#ifdef NEOPIXEL_RGBW
					if (frameState.protocolVersion2)
					{
						if (calibrationConfig.red != incoming.red || calibrationConfig.green != incoming.green ||
							calibrationConfig.blue != incoming.blue || calibrationConfig.gain != incoming.gain)
						{
							calibrationConfig.setParams(incoming.gain, incoming.red, incoming.green, incoming.blue);
							calibrationConfig.prepareCalibration();
						}
					}
				#endif
			}

			frameState.state = AwaProtocol::HEADER_A;
			break;
		}
	}
}

void setup()
{
	// Init serial port
	Serial.begin(SERIALCOM_SPEED);
	Serial.setTimeout(50);
	Serial.setRxBufferSize(2048);

	// Display config
	Serial.println("\r\nWelcome!\r\nAwa driver 7.");

	// Colorspace/Led type info
	#if defined(NEOPIXEL_RGBW) || defined(NEOPIXEL_RGB)
		#ifdef NEOPIXEL_RGBW
			#ifdef COLD_WHITE
				Serial.println("NeoPixelBus SK6812 cold GRBW.");
			#else
				Serial.println("NeoPixelBus SK6812 neutral GRBW.");
			#endif
			calibrationConfig.prepareCalibration();
			printCalibration();
		#else
			Serial.println("NeoPixelBus ws281x type (GRB).");
		#endif
	#elif defined(SPILED_APA102)
		Serial.println("SPI APA102 compatible type (BGR).");
	#elif defined(SPILED_WS2801)
		Serial.println("SPI WS2801 (RBG).");
	#endif
}

void loop()
{
	readSerialData();
}