#include <nan.h>
#include <list>
#include <vector>
#include "./serialport.h"
#include "win/disphelper.h"
#include "win/stdafx.h"
#include "win/enumser.h"

#ifdef WIN32

#define MAX_BUFFER_SIZE 1000

struct WindowsPlatformOptions : OpenBatonPlatformOptions {
};

OpenBatonPlatformOptions* ParsePlatformOptions(const v8::Local<v8::Object>& options) {
  // currently none
  return new WindowsPlatformOptions();
}

// Declare type of pointer to CancelIoEx function
typedef BOOL (WINAPI *CancelIoExType)(HANDLE hFile, LPOVERLAPPED lpOverlapped);

std::list<int> g_closingHandles;
int bufferSize;
void ErrorCodeToString(const char* prefix, int errorCode, char *errorStr) {
  switch (errorCode) {
  case ERROR_FILE_NOT_FOUND:
    _snprintf_s(errorStr, ERROR_STRING_SIZE, _TRUNCATE, "%s: File not found", prefix);
    break;
  case ERROR_INVALID_HANDLE:
    _snprintf_s(errorStr, ERROR_STRING_SIZE, _TRUNCATE, "%s: Invalid handle", prefix);
    break;
  case ERROR_ACCESS_DENIED:
    _snprintf_s(errorStr, ERROR_STRING_SIZE, _TRUNCATE, "%s: Access denied", prefix);
    break;
  case ERROR_OPERATION_ABORTED:
    _snprintf_s(errorStr, ERROR_STRING_SIZE, _TRUNCATE, "%s: operation aborted", prefix);
    break;
  default:
    _snprintf_s(errorStr, ERROR_STRING_SIZE, _TRUNCATE, "%s: Unknown error code %d", prefix, errorCode);
    break;
  }
}

void EIO_Open(uv_work_t* req) {
  OpenBaton* data = static_cast<OpenBaton*>(req->data);

  char originalPath[1024];
  strncpy_s(originalPath, sizeof(originalPath), data->path, _TRUNCATE);
  // data->path is char[1024] but on Windows it has the form "COMx\0" or "COMxx\0"
  // We want to prepend "\\\\.\\" to it before we call CreateFile
  strncpy(data->path + 20, data->path, 10);
  strncpy(data->path, "\\\\.\\", 4);
  strncpy(data->path + 4, data->path + 20, 10);

  int shareMode = FILE_SHARE_READ | FILE_SHARE_WRITE;
  if (data->lock) {
    shareMode = 0;
  }

  HANDLE file = CreateFile(
    data->path,
    GENERIC_READ | GENERIC_WRITE,
    shareMode,  // dwShareMode 0 Prevents other processes from opening if they request delete, read, or write access
    NULL,
    OPEN_EXISTING,
    FILE_FLAG_OVERLAPPED,  // allows for reading and writing at the same time and sets the handle for asynchronous I/O
    NULL
  );

  if (file == INVALID_HANDLE_VALUE) {
    DWORD errorCode = GetLastError();
    char temp[100];
    _snprintf_s(temp, sizeof(temp), _TRUNCATE, "Opening %s", originalPath);
    ErrorCodeToString(temp, errorCode, data->errorString);
    return;
  }

  bufferSize = data->bufferSize;
  if (bufferSize > MAX_BUFFER_SIZE) {
    bufferSize = MAX_BUFFER_SIZE;
  }

  DCB dcb = { 0 };
  SecureZeroMemory(&dcb, sizeof(DCB));
  dcb.DCBlength = sizeof(DCB);

  if (!GetCommState(file, &dcb)) {
    ErrorCodeToString("Open (GetCommState)", GetLastError(), data->errorString);
    CloseHandle(file);
    return;
  }

  if (data->hupcl) {
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
  } else {
    dcb.fDtrControl = DTR_CONTROL_DISABLE;  // disable DTR to avoid reset
  }

  dcb.Parity = NOPARITY;
  dcb.ByteSize = 8;
  dcb.StopBits = ONESTOPBIT;
  dcb.fInX = FALSE;
  dcb.fOutX = FALSE;
  dcb.fOutxDsrFlow = FALSE;
  dcb.fOutxCtsFlow = FALSE;
  dcb.fRtsControl = RTS_CONTROL_ENABLE;

  dcb.fBinary = true;
  dcb.BaudRate = data->baudRate;
  dcb.ByteSize = data->dataBits;

  switch (data->parity) {
  case SERIALPORT_PARITY_NONE:
    dcb.Parity = NOPARITY;
    break;
  case SERIALPORT_PARITY_MARK:
    dcb.Parity = MARKPARITY;
    break;
  case SERIALPORT_PARITY_EVEN:
    dcb.Parity = EVENPARITY;
    break;
  case SERIALPORT_PARITY_ODD:
    dcb.Parity = ODDPARITY;
    break;
  case SERIALPORT_PARITY_SPACE:
    dcb.Parity = SPACEPARITY;
    break;
  }

  switch (data->stopBits) {
  case SERIALPORT_STOPBITS_ONE:
    dcb.StopBits = ONESTOPBIT;
    break;
  case SERIALPORT_STOPBITS_ONE_FIVE:
    dcb.StopBits = ONE5STOPBITS;
    break;
  case SERIALPORT_STOPBITS_TWO:
    dcb.StopBits = TWOSTOPBITS;
    break;
  }

  if (!SetCommState(file, &dcb)) {
    ErrorCodeToString("Open (SetCommState)", GetLastError(), data->errorString);
    CloseHandle(file);
    return;
  }

  // Clear all timeouts for read and write operations
  COMMTIMEOUTS commTimeouts = {0};
  commTimeouts.ReadIntervalTimeout = 0;  // Minimize chance of concatenating of separate serial port packets on read
  commTimeouts.ReadTotalTimeoutMultiplier = 0;  // Do not allow big read timeout when big read buffer used
  commTimeouts.ReadTotalTimeoutConstant = 0;  // Total read timeout (period of read loop)
  commTimeouts.WriteTotalTimeoutConstant = 0;  // Const part of write timeout
  commTimeouts.WriteTotalTimeoutMultiplier = 0;  // Variable part of write timeout (per byte)

  if (!SetCommTimeouts(file, &commTimeouts)) {
    ErrorCodeToString("Open (SetCommTimeouts)", GetLastError(), data->errorString);
    CloseHandle(file);
    return;
  }

  // Remove garbage data in RX/TX queues
  PurgeComm(file, PURGE_RXCLEAR);
  PurgeComm(file, PURGE_TXCLEAR);

  data->result = (int)file;
}

struct WatchPortBaton {
  HANDLE fd;
  DWORD bytesRead;
  char buffer[MAX_BUFFER_SIZE];
  char errorString[ERROR_STRING_SIZE];
  DWORD errorCode;
  bool disconnected;
  Nan::Callback* dataCallback;
  Nan::Callback* errorCallback;
  Nan::Callback* disconnectedCallback;
};

void EIO_Update(uv_work_t* req) {
  ConnectionOptionsBaton* data = static_cast<ConnectionOptionsBaton*>(req->data);

  DCB dcb = { 0 };
  SecureZeroMemory(&dcb, sizeof(DCB));
  dcb.DCBlength = sizeof(DCB);

  if (!GetCommState((HANDLE)data->fd, &dcb)) {
    ErrorCodeToString("GetCommState", GetLastError(), data->errorString);
    return;
  }

  dcb.BaudRate = data->baudRate;

  if (!SetCommState((HANDLE)data->fd, &dcb)) {
    ErrorCodeToString("SetCommState", GetLastError(), data->errorString);
    return;
  }
}

void EIO_Set(uv_work_t* req) {
  SetBaton* data = static_cast<SetBaton*>(req->data);

  if (data->rts) {
    EscapeCommFunction((HANDLE)data->fd, SETRTS);
  } else {
    EscapeCommFunction((HANDLE)data->fd, CLRRTS);
  }

  if (data->dtr) {
    EscapeCommFunction((HANDLE)data->fd, SETDTR);
  } else {
    EscapeCommFunction((HANDLE)data->fd, CLRDTR);
  }

  if (data->brk) {
    EscapeCommFunction((HANDLE)data->fd, SETBREAK);
  } else {
    EscapeCommFunction((HANDLE)data->fd, CLRBREAK);
  }

  DWORD bits = 0;

  GetCommMask((HANDLE)data->fd, &bits);

  bits &= ~(EV_CTS | EV_DSR);

  if (data->cts) {
    bits |= EV_CTS;
  }

  if (data->dsr) {
    bits |= EV_DSR;
  }

  if (!SetCommMask((HANDLE)data->fd, bits)) {
    ErrorCodeToString("Setting options on COM port (SetCommMask)", GetLastError(), data->errorString);
    return;
  }
}

void EIO_Get(uv_work_t* req) {
  GetBaton* data = static_cast<GetBaton*>(req->data);

  DWORD bits = 0;
  if (!GetCommModemStatus((HANDLE)data->fd, &bits)) {
    ErrorCodeToString("Getting control settings on COM port (GetCommModemStatus)", GetLastError(), data->errorString);
    return;
  }

  data->cts = bits & MS_CTS_ON;
  data->dsr = bits & MS_DSR_ON;
  data->dcd = bits & MS_RLSD_ON;
}

void EIO_WatchPort(uv_work_t* req) {
  WatchPortBaton* data = static_cast<WatchPortBaton*>(req->data);
  data->bytesRead = 0;
  data->disconnected = false;

  // Event used by GetOverlappedResult(..., TRUE) to wait for incoming data or timeout
  // Event MUST be used if program has several simultaneous asynchronous operations
  // on the same handle (i.e. ReadFile and WriteFile)
  HANDLE hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

  while (true) {
    OVERLAPPED ov = {0};
    ov.hEvent = hEvent;

    // Start read operation - synchrounous or asynchronous
    DWORD bytesReadSync = 0;
    if (!ReadFile((HANDLE)data->fd, data->buffer, bufferSize, &bytesReadSync, &ov)) {
      data->errorCode = GetLastError();
      if (data->errorCode != ERROR_IO_PENDING) {
        // Read operation error
        if (data->errorCode == ERROR_OPERATION_ABORTED) {
          data->disconnected = true;
        } else {
          ErrorCodeToString("Reading from COM port (ReadFile)", data->errorCode, data->errorString);
          CloseHandle(hEvent);
          return;
        }
        break;
      }

      // Read operation is asynchronous and is pending
      // We MUST wait for operation completion before deallocation of OVERLAPPED struct
      // or read data buffer

      // Wait for async read operation completion or timeout
      DWORD bytesReadAsync = 0;
      if (!GetOverlappedResult((HANDLE)data->fd, &ov, &bytesReadAsync, TRUE)) {
        // Read operation error
        data->errorCode = GetLastError();
        if (data->errorCode == ERROR_OPERATION_ABORTED) {
          data->disconnected = true;
        } else {
          ErrorCodeToString("Reading from COM port (GetOverlappedResult)", data->errorCode, data->errorString);
          CloseHandle(hEvent);
          return;
        }
        break;
      } else {
        // Read operation completed asynchronously
        data->bytesRead = bytesReadAsync;
      }
    } else {
      // Read operation completed synchronously
      data->bytesRead = bytesReadSync;
    }

    // Return data received if any
    if (data->bytesRead > 0) {
      break;
    }
  }

  CloseHandle(hEvent);
}

bool IsClosingHandle(int fd) {
  for (std::list<int>::iterator it = g_closingHandles.begin(); it != g_closingHandles.end(); ++it) {
    if (fd == *it) {
      g_closingHandles.remove(fd);
      return true;
    }
  }
  return false;
}

void DisposeWatchPortCallbacks(WatchPortBaton* data) {
  delete data->dataCallback;
  delete data->errorCallback;
  delete data->disconnectedCallback;
}

// FinalizerCallback will prevent WatchPortBaton::buffer from getting
// collected by gc while finalizing v8::ArrayBuffer. The buffer will
// get cleaned up through this callback.
static void FinalizerCallback(char* data, void* hint) {
  uv_work_t* req = reinterpret_cast<uv_work_t*>(hint);
  WatchPortBaton* wpb = static_cast<WatchPortBaton*>(req->data);
  delete wpb;
  delete req;
}

void EIO_AfterWatchPort(uv_work_t* req) {
  Nan::HandleScope scope;

  WatchPortBaton* data = static_cast<WatchPortBaton*>(req->data);
  if (data->disconnected) {
    data->disconnectedCallback->Call(0, NULL);
    DisposeWatchPortCallbacks(data);
    goto cleanup;
  }

  bool skipCleanup = false;
  if (data->bytesRead > 0) {
    v8::Local<v8::Value> argv[1];
    argv[0] = Nan::NewBuffer(data->buffer, data->bytesRead, FinalizerCallback, req).ToLocalChecked();
    skipCleanup = true;
    data->dataCallback->Call(1, argv);
  } else if (data->errorCode > 0) {
    if (data->errorCode == ERROR_INVALID_HANDLE && IsClosingHandle((int)data->fd)) {
      DisposeWatchPortCallbacks(data);
      goto cleanup;
    } else {
      v8::Local<v8::Value> argv[1];
      argv[0] = Nan::Error(data->errorString);
      data->errorCallback->Call(1, argv);
      Sleep(100);  // prevent the errors from occurring too fast
    }
  }
  AfterOpenSuccess((int)data->fd, data->dataCallback, data->disconnectedCallback, data->errorCallback);

cleanup:
  if (!skipCleanup) {
    delete data;
    delete req;
  }
}

void AfterOpenSuccess(int fd, Nan::Callback* dataCallback, Nan::Callback* disconnectedCallback, Nan::Callback* errorCallback) {
  WatchPortBaton* baton = new WatchPortBaton();
  memset(baton, 0, sizeof(WatchPortBaton));
  baton->fd = (HANDLE)fd;
  baton->dataCallback = dataCallback;
  baton->errorCallback = errorCallback;
  baton->disconnectedCallback = disconnectedCallback;

  uv_work_t* req = new uv_work_t();
  req->data = baton;

  uv_queue_work(uv_default_loop(), req, EIO_WatchPort, (uv_after_work_cb)EIO_AfterWatchPort);
}

void EIO_Write(uv_work_t* req) {
  QueuedWrite* queuedWrite = static_cast<QueuedWrite*>(req->data);
  WriteBaton* data = static_cast<WriteBaton*>(queuedWrite->baton);
  data->result = 0;

  do {
    OVERLAPPED ov = {0};
    // Event used by GetOverlappedResult(..., TRUE) to wait for outgoing data or timeout
    // Event MUST be used if program has several simultaneous asynchronous operations
    // on the same handle (i.e. ReadFile and WriteFile)
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    // Start write operation - synchronous or asynchronous
    DWORD bytesWritten = 0;
    if (!WriteFile((HANDLE)data->fd, data->bufferData, static_cast<DWORD>(data->bufferLength), &bytesWritten, &ov)) {
      DWORD lastError = GetLastError();
      if (lastError != ERROR_IO_PENDING) {
        // Write operation error
        ErrorCodeToString("Writing to COM port (WriteFile)", lastError, data->errorString);
        CloseHandle(ov.hEvent);
        return;
      }
      // Write operation is completing asynchronously
      // We MUST wait for the operation completion before deallocation of OVERLAPPED struct
      // or write data buffer

      // block for async write operation completion
      bytesWritten = 0;
      if (!GetOverlappedResult((HANDLE)data->fd, &ov, &bytesWritten, TRUE)) {
        // Write operation error
        DWORD lastError = GetLastError();
        ErrorCodeToString("Writing to COM port (GetOverlappedResult)", lastError, data->errorString);
        CloseHandle(ov.hEvent);
        return;
      }
    }
    // Write operation completed synchronously
    data->result = bytesWritten;
    data->offset += data->result;
    CloseHandle(ov.hEvent);
  } while (data->bufferLength > data->offset);
}

void EIO_Close(uv_work_t* req) {
  VoidBaton* data = static_cast<VoidBaton*>(req->data);

  g_closingHandles.push_back(data->fd);

  HMODULE hKernel32 = LoadLibrary("kernel32.dll");
  // Look up function address
  CancelIoExType pCancelIoEx = (CancelIoExType)GetProcAddress(hKernel32, "CancelIoEx");
  // Do something with it
  if (pCancelIoEx) {
    // Function exists so call it
    // Cancel all pending IO Requests for the current device
    pCancelIoEx((HANDLE)data->fd, NULL);
  }
  if (!CloseHandle((HANDLE)data->fd)) {
    ErrorCodeToString("closing connection", GetLastError(), data->errorString);
    return;
  }
}

/*
 * listComPorts.c -- list COM ports
 *
 * http://github.com/todbot/usbSearch/
 *
 * 2012, Tod E. Kurt, http://todbot.com/blog/
 *
 *
 * Uses DispHealper : http://disphelper.sourceforge.net/
 *
 * Notable VIDs & PIDs combos:
 * VID 0403 - FTDI
 *
 * VID 0403 / PID 6001 - Arduino Diecimila
 *
 */
void EIO_List(uv_work_t* req) {
  ListBaton* data = static_cast<ListBaton*>(req->data);

  {
    DISPATCH_OBJ(wmiSvc);
    DISPATCH_OBJ(colDevices);

    dhInitialize(TRUE);
    dhToggleExceptions(FALSE);

    dhGetObject(L"winmgmts:{impersonationLevel=impersonate}!\\\\.\\root\\cimv2", NULL, &wmiSvc);
    dhGetValue(L"%o", &colDevices, wmiSvc, L".ExecQuery(%S)", L"Select * from Win32_PnPEntity");

    int port_count = 0;
    FOR_EACH(objDevice, colDevices, NULL) {
      char* name = NULL;
      char* pnpid = NULL;
      char* manu = NULL;
      char* match;

      dhGetValue(L"%s", &name,  objDevice, L".Name");
      dhGetValue(L"%s", &pnpid, objDevice, L".PnPDeviceID");

      if (name != NULL && ((match = strstr(name, "(COM")) != NULL)) {  // look for "(COM23)"
        // 'Manufacturuer' can be null, so only get it if we need it
        dhGetValue(L"%s", &manu, objDevice,  L".Manufacturer");
        port_count++;
        char* comname = strtok(match, "()");
        ListResultItem* resultItem = new ListResultItem();
        resultItem->comName = comname;
        resultItem->manufacturer = manu;
        resultItem->pnpId = pnpid;
        data->results.push_back(resultItem);
        dhFreeString(manu);
      }

      dhFreeString(name);
      dhFreeString(pnpid);
    } NEXT(objDevice);

    SAFE_RELEASE(colDevices);
    SAFE_RELEASE(wmiSvc);

    dhUninitialize(TRUE);
  }

  std::vector<UINT> ports;
  if (CEnumerateSerial::UsingQueryDosDevice(ports)) {
    for (size_t i = 0; i < ports.size(); i++) {
      char comname[64] = { 0 };
      _snprintf_s(comname, sizeof(comname), _TRUNCATE, "COM%u", ports[i]);
      bool bFound = false;
      for (std::list<ListResultItem*>::iterator ri = data->results.begin(); ri != data->results.end(); ++ri) {
        if (stricmp((*ri)->comName.c_str(), comname) == 0) {
          bFound = true;
          break;
        }
      }
      if (!bFound) {
        ListResultItem* resultItem = new ListResultItem();
        resultItem->comName = comname;
        resultItem->manufacturer = "";
        resultItem->pnpId = "";
        data->results.push_back(resultItem);
      }
    }
  }
}

void EIO_Flush(uv_work_t* req) {
  VoidBaton* data = static_cast<VoidBaton*>(req->data);

  DWORD purge_all = PURGE_RXABORT | PURGE_RXCLEAR | PURGE_TXABORT | PURGE_TXCLEAR;
  if (!PurgeComm((HANDLE)data->fd, purge_all)) {
    ErrorCodeToString("flushing connection (PurgeComm)", GetLastError(), data->errorString);
    return;
  }
}

void EIO_Drain(uv_work_t* req) {
  VoidBaton* data = static_cast<VoidBaton*>(req->data);

  if (!FlushFileBuffers((HANDLE)data->fd)) {
    ErrorCodeToString("draining connection (FlushFileBuffers)", GetLastError(), data->errorString);
    return;
  }
}

#endif
