// Purpose: MJPEG stream reader that extracts JPEG frames from a file buffer for playback.
#pragma once

#include <Arduino.h>
#include <FS.h>
#include <JPEGDEC.h>
#include <cstring>

#define MJPEG_READ_BATCH_SIZE (32 * 1024)

class MjpegClass
{
public:
  bool setup(
      File *inputFile,
      uint8_t *mjpeg_buf,
      size_t mjpeg_buf_size,
      JPEG_DRAW_CALLBACK *pfnDraw,
      bool useBigEndian,
      int x,
      int y,
      int widthLimit,
      int heightLimit)
  {
    return setupInternal(
        inputFile,
        inputFile,
        mjpeg_buf,
        mjpeg_buf_size,
        pfnDraw,
        useBigEndian,
        x,
        y,
        widthLimit,
        heightLimit);
  }

  bool setup(
      Stream *input,
      uint8_t *mjpeg_buf,
      size_t mjpeg_buf_size,
      JPEG_DRAW_CALLBACK *pfnDraw,
      bool useBigEndian,
      int x,
      int y,
      int widthLimit,
      int heightLimit)
  {
    return setupInternal(
        input,
        nullptr,
        mjpeg_buf,
        mjpeg_buf_size,
        pfnDraw,
        useBigEndian,
        x,
        y,
        widthLimit,
        heightLimit);
  }

  bool readMjpegBuf()
  {
    _frameLen = 0;

    while (true)
    {
      if ((_tail - _head) < 2)
      {
        if (readBytesToTail() <= 0)
        {
          return false;
        }
      }

      if (!_haveSoi)
      {
        if (_soiScanPos < _head)
        {
          _soiScanPos = _head;
        }

        int32_t soi = findMarker(_soiScanPos, 0xD8);
        if (soi < 0)
        {
          if ((_tail > _head) && (_mjpeg_buf[_tail - 1] == 0xFF))
          {
            _head = _tail - 1;
          }
          else
          {
            _head = _tail;
          }

          _soiScanPos = _head;
          _eoiScanPos = _head;

          if (_head == _tail)
          {
            _head = 0;
            _tail = 0;
            _soiScanPos = 0;
            _eoiScanPos = 0;
          }

          if (readBytesToTail() <= 0)
          {
            return false;
          }
          continue;
        }

        _head = soi;
        _haveSoi = true;
        _eoiScanPos = _head + 2;
      }

      if (_eoiScanPos < (_head + 2))
      {
        _eoiScanPos = _head + 2;
      }

      int32_t eoi = findMarker(_eoiScanPos, 0xD9);
      if (eoi < 0)
      {
        if ((_tail - _head) >= (int32_t)_mjpeg_buf_size)
        {
          return false;
        }

        if (_tail > _head)
        {
          int32_t nextScan = _tail - 1;
          if (nextScan > _eoiScanPos)
          {
            _eoiScanPos = nextScan;
          }
        }

        if (readBytesToTail() <= 0)
        {
          return false;
        }
        continue;
      }

      _frameStart = _head;
      _frameLen = (eoi + 2) - _head;
      _head = eoi + 2;
      _haveSoi = false;
      _soiScanPos = _head;
      _eoiScanPos = _head;

      if (_head == _tail)
      {
        _head = 0;
        _tail = 0;
        _soiScanPos = 0;
        _eoiScanPos = 0;
      }

      return true;
    }
  }

  bool drawJpg()
  {
    if (_frameLen <= 0)
    {
      return false;
    }

    _jpeg.openRAM(_mjpeg_buf + _frameStart, _frameLen, _pfnDraw);

    if (_useBigEndian)
    {
      _jpeg.setPixelType(RGB565_BIG_ENDIAN);
    }

    _jpeg.decode(_baseX, _baseY, 0);
    _jpeg.close();

    return true;
  }

  // Decode the current frame centered on a screen of the given size.
  bool drawJpgCentered(int screenWidth, int screenHeight)
  {
    return drawJpgBuffer(_mjpeg_buf + _frameStart, _frameLen, screenWidth, screenHeight);
  }

  // Decode an arbitrary in-RAM JPEG frame centered on the screen. Used by the
  // dual-core pipeline so the decode side never touches the reader's buffer.
  // Safe to call from a different core than readMjpegBuf(): it only reads the
  // caller-owned jpgData plus the immutable _pfnDraw/_useBigEndian fields.
  bool drawJpgBuffer(uint8_t *jpgData, int32_t jpgLen, int screenWidth, int screenHeight)
  {
    if (jpgData == nullptr || jpgLen <= 0)
    {
      return false;
    }

    if (!_jpeg.openRAM(jpgData, jpgLen, _pfnDraw))
    {
      return false;
    }

    if (_useBigEndian)
    {
      _jpeg.setPixelType(RGB565_BIG_ENDIAN);
    }

    int frameW = _jpeg.getWidth();
    int frameH = _jpeg.getHeight();
    int drawX = (screenWidth - frameW) / 2;
    int drawY = (screenHeight - frameH) / 2;
    if (drawX < 0)
    {
      drawX = 0;
    }
    if (drawY < 0)
    {
      drawY = 0;
    }

    _jpeg.decode(drawX, drawY, 0);
    _jpeg.close();

    return true;
  }

  // Current frame extracted by readMjpegBuf(), as a pointer into the reader buffer.
  uint8_t *frameData() { return _mjpeg_buf + _frameStart; }
  int32_t frameLen() const { return _frameLen; }

private:
  bool setupInternal(
      Stream *inputStream,
      File *inputFile,
      uint8_t *mjpeg_buf,
      size_t mjpeg_buf_size,
      JPEG_DRAW_CALLBACK *pfnDraw,
      bool useBigEndian,
      int x,
      int y,
      int widthLimit,
      int heightLimit)
  {
    if (inputStream == nullptr || mjpeg_buf == nullptr || pfnDraw == nullptr || mjpeg_buf_size < 4)
    {
      return false;
    }

    _inputStream = inputStream;
    _inputFile = inputFile;
    _mjpeg_buf = mjpeg_buf;
    _mjpeg_buf_size = mjpeg_buf_size;
    _pfnDraw = pfnDraw;
    _useBigEndian = useBigEndian;
    _baseX = x;
    _baseY = y;
    _widthLimit = widthLimit;
    _heightLimit = heightLimit;
    _head = 0;
    _tail = 0;
    _soiScanPos = 0;
    _eoiScanPos = 0;
    _haveSoi = false;
    _frameStart = 0;
    _frameLen = 0;

    return true;
  }

  int32_t readBytesToTail()
  {
    if (_tail >= (int32_t)_mjpeg_buf_size)
    {
      if (_head <= 0)
      {
        return 0;
      }

      int32_t oldHead = _head;
      int32_t remaining = _tail - oldHead;
      if (remaining > 0)
      {
        memmove(_mjpeg_buf, _mjpeg_buf + oldHead, (size_t)remaining);
      }
      _head = 0;
      _tail = remaining;

      if (_haveSoi)
      {
        _eoiScanPos -= oldHead;
        if (_eoiScanPos < (_head + 2))
        {
          _eoiScanPos = _head + 2;
        }
      }
      else
      {
        _soiScanPos -= oldHead;
        if (_soiScanPos < _head)
        {
          _soiScanPos = _head;
        }
      }
    }

    int32_t freeSpace = (int32_t)_mjpeg_buf_size - _tail;
    if (freeSpace <= 0)
    {
      return 0;
    }

    int32_t readLen = readBytesTo(_mjpeg_buf + _tail, freeSpace);
    if (readLen > 0)
    {
      _tail += readLen;
    }
    return readLen;
  }

  int32_t findMarker(int32_t from, uint8_t markerSecondByte) const
  {
    if (from < _head)
    {
      from = _head;
    }

    for (int32_t i = from; (i + 1) < _tail; ++i)
    {
      if ((_mjpeg_buf[i] == 0xFF) && (_mjpeg_buf[i + 1] == markerSecondByte))
      {
        return i;
      }
    }

    return -1;
  }

  int32_t readBytesTo(uint8_t *dest, int32_t capacity)
  {
    if (capacity <= 0)
    {
      return 0;
    }

    int32_t toRead = (capacity < MJPEG_READ_BATCH_SIZE) ? capacity : MJPEG_READ_BATCH_SIZE;
    if (_inputFile != nullptr)
    {
      return (int32_t)_inputFile->read(dest, (size_t)toRead);
    }
    return (int32_t)_inputStream->readBytes(dest, (size_t)toRead);
  }

  Stream *_inputStream = nullptr;
  File *_inputFile = nullptr;
  uint8_t *_mjpeg_buf = nullptr;
  size_t _mjpeg_buf_size = 0;
  JPEG_DRAW_CALLBACK *_pfnDraw = nullptr;
  bool _useBigEndian = false;
  int _baseX = 0;
  int _baseY = 0;
  int _widthLimit = 0;
  int _heightLimit = 0;

  JPEGDEC _jpeg;
  int32_t _head = 0;
  int32_t _tail = 0;
  int32_t _soiScanPos = 0;
  int32_t _eoiScanPos = 0;
  bool _haveSoi = false;
  int32_t _frameStart = 0;
  int32_t _frameLen = 0;
};
