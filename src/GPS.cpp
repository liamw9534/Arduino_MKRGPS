/*
  This file is part of the Arduino_MKRGPS library.
  Copyright (c) 2019 Arduino SA. All rights reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

// Useful info on NMEA data: https://www.gpsinformation.org/dale/nmea.htm

extern "C" {
  #include "minmea/minmea.h"
}

// #define GPS_DEBUG

#include "GPS.h"

#ifdef SHARED_PORT
#include "../../../../../include/SharedSerial.h"
#endif

#define GPS_MASK_RMC 0x01
#define GPS_MASK_GGA 0x02

GPSClass::GPSClass(HardwareSerial& serial, unsigned long baudrate, SerialDDC& serialDDC, unsigned long clockrate, int extintPin) :
  _serial(&serial),
  _baudrate(baudrate),
  _serialDDC(&serialDDC),
  _clockRate(clockrate),
  _extintPin(extintPin)
{
}

GPSClass::~GPSClass()
{
}

int GPSClass::begin(int mode)
{
  _mode = mode;

  pinMode(_extintPin, OUTPUT);
  digitalWrite(_extintPin, HIGH);
  delay(100); // delay for GPS to wakeup

  if (_mode == GPS_MODE_UART) {
    _serial->begin(_baudrate);
    _stream = _serial;
  } else {
    if (!_serialDDC->begin(_clockRate)) {
      end();

      return 0;
    }

    _stream = _serialDDC;
  }

  _ts.tv_sec = 0;
  _available = 0;
  _index = 0;

  return 1;
}

void GPSClass::end()
{
  if (_mode == GPS_MODE_UART) {
    _serial->end();
    pinMode(13, INPUT_PULLUP);
    pinMode(14, INPUT_PULLUP);
  } else {
    _serialDDC->end();
  }
  delay(10);
  digitalWrite(_extintPin, LOW);
#ifndef GPS_EXTINT_NOPULL
  pinMode(_extintPin, INPUT);
#endif
}

int GPSClass::available()
{
  poll();

  if (_available == (GPS_MASK_RMC | GPS_MASK_GGA)) {
    _available = 0x00;

    return 1;
  }

  return 0;
}

float GPSClass::latitude()
{
  if (isnan(_latitude)) {
    return NAN;
  }

  return toDegrees(_latitude);
}

float GPSClass::longitude()
{
  if (isnan(_longitude)) {
    return NAN;
  }

  return toDegrees(_longitude);
}

float GPSClass::speed()
{
  // convert speed from knots to kph
  return _speed * 1.852;
}

float GPSClass::course()
{
  return _course;
}

float GPSClass::variation()
{
  if (isnan(_variation)) {
    return NAN;
  }

  return toDegrees(_variation);
}

float GPSClass::altitude()
{
  return _altitude;
}

int GPSClass::satellites()
{
  return _satellites;
}

unsigned long GPSClass::getTime()
{
  return _ts.tv_sec;
}

bool GPSClass::prepareStandby()
{
  byte payload[48];

  memset(payload, 0x00, sizeof(payload));

  payload[0] = 0x02; // version 2
  // flags:
  //       extintSel    = EXTINT0
  //       extintWake   = enabled, keep receiver awake as long as selected EXTINT pin is 'high'
  //       extintBackup = enabled, force receiver into BACKUP mode when selected EXTINT pin is 'low' 
  payload[4] = 0x60;

  int retries = 5;

  // Flush input buffer
  while (_stream->available()) {
    (void)_stream->read();
  }

  while (retries-- > 0) {
    sendUbx(0x06, 0x3b, payload, sizeof(payload));
    delay(300);
    while (_stream->available()) {
      if (_stream->read() == 0xb5 &&
          _stream->read() == 0x62 &&
          _stream->read() == 0x05 &&
          _stream->read() == 0x01)
        return true;
    }
  }
  return false;
#if 0
  if (_mode == GPS_MODE_UART) {
    _serial->end();
  } else {
    _serialDDC->end();
  }

  digitalWrite(_extintPin, LOW);

  _available = 0;
  _index = 0;
#endif
}

void GPSClass::wakeup()
{
  digitalWrite(_extintPin, HIGH);
  delay(100); // delay for GPS to wakeup

  if (_mode == GPS_MODE_UART) {
    _serial->begin(_baudrate);
  } else {
    _serialDDC->begin(_clockRate);
  }

  _available = 0;
  _index = 0;
}

void GPSClass::poll()
{
  while (_stream->available()) {
    char c = _stream->read();

#ifdef GPS_DEBUG
    DEBUG_PORT.print("GPS -> App: ");
    DEBUG_PORT.print(c);
    DEBUG_PORT.print("\n");
#endif

    if (c == '$') {
      _index = 0;
    }

    if (_index < ((int)sizeof(_buffer) - 1)) {
      _buffer[_index++] = c;

      if (c == '\n' && _buffer[0] == '$') {
        _buffer[_index++] = '\0';

        parseBuffer();
      }
    }
  }
}

void GPSClass::parseBuffer()
{
  int sentenceId = minmea_sentence_id(_buffer, false);

  switch (sentenceId) {
    case MINMEA_SENTENCE_RMC: {
      struct minmea_sentence_rmc frame;
 
      if (minmea_parse_rmc(&frame, _buffer)) {
        _latitude = minmea_tofloat(&frame.latitude);
        _longitude = minmea_tofloat(&frame.longitude);
        _speed = minmea_tofloat(&frame.speed);
        _course = minmea_tofloat(&frame.course);
        _variation = minmea_tofloat(&frame.variation);
        _ts.tv_sec = 0;
        minmea_gettime(&_ts, &frame.date, &frame.time);
        _available |= (GPS_MASK_RMC * frame.valid);
      }
      break;
    }

    case MINMEA_SENTENCE_GGA: {
      struct minmea_sentence_gga frame;
      
      if (minmea_parse_gga(&frame, _buffer) && frame.fix_quality != 0) {
        _latitude = minmea_tofloat(&frame.latitude);
        _longitude = minmea_tofloat(&frame.longitude);
        _altitude = minmea_tofloat(&frame.altitude);
        _satellites = frame.satellites_tracked;

        _available |= GPS_MASK_GGA;
      }

      break;
    }

    default:
      break;
  }
}

void GPSClass::sendUbx(uint8_t cls, uint8_t id, uint8_t payload[], uint16_t length)
{
  uint8_t ckA = 0;
  uint8_t ckB = 0;

  unsigned int cmdLength = 8 + length;
  uint8_t cmd[cmdLength];

  cmd[0] = 0xb5;
  cmd[1] = 0x62;
  cmd[2] = cls;
  cmd[3] = id;
  cmd[4] = (length & 0xff);
  cmd[5] = (length >> 8);
  memcpy(&cmd[6], payload, length);

  // calculate checksum, start at index 2 and up to the payload
  for (unsigned int i = 2; i < (cmdLength - 2); i++) {
    ckA += cmd[i];
    ckB += ckA;
  }

  cmd[cmdLength - 2] = ckA;
  cmd[cmdLength - 1] = ckB;

  _stream->write(cmd, cmdLength);
  _stream->flush();
}

float GPSClass::toDegrees(float f)
{
  float degrees = (int)(f / 100.0);
  float minutes = f - (100.0 * degrees);

  return (degrees + (minutes / 60.0));
}

static SerialDDC serialDDC(Wire, 0x42, 0xfd, 0xff);

GPSClass GPS(GPS_PORT, 9600, serialDDC, 400000, 7);
