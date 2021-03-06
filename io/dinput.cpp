
#include "dinput.h"

#define DIRECTINPUT_VERSION 0x0800

#include <dinput.h>
#include <windows.h>

#include <XInput.h>

#ifndef XUSER_MAX_COUNT
#define XUSER_MAX_COUNT 4
#endif

#include <wbemidl.h>

#include <string>

#include <assert.h>

#define _USE_MATH_DEFINES
#include <math.h>

#include "guid_container.h"

#pragma comment(lib, "WbemUuid.lib")

class dinput_i : public dinput {
  guid_container *guids;

  LPDIRECTINPUTDEVICE8 lpdKeyboard;
  LPDIRECTINPUTDEVICE8 lpdMouse;
  DIMOUSESTATE m_mouseState;

  mouse_t mousie;

  struct joy {
    unsigned serial;
    LPDIRECTINPUTDEVICE8 device;
    unsigned long vidpid;
#ifdef UNICODE
    std::wstring name;
#else
    std::string name;
#endif

    joy(unsigned serial, const unsigned long &GuidInstance, const TCHAR *name,
        LPDIRECTINPUTDEVICE8 device) {
      this->serial = serial;
      this->device = device;
      this->vidpid = GuidInstance;
      this->name = name;
    }
  };

  std::vector<joy> joysticks;

  bool focused;

  // only needed by enum callbacks
  LPDIRECTINPUT8 lpDI;

  // typedef void (WINAPI * tXInputEnable)(BOOL);
  typedef DWORD(WINAPI *tXInputGetState)(DWORD, XINPUT_STATE *);

  HMODULE hXInput;
  // tXInputEnable            pXInputEnable;
  tXInputGetState pXInputGetState;

  XINPUT_STATE xinput_last_state[XUSER_MAX_COUNT];

  typedef UINT(WINAPI *tGetRawInputDeviceList)(
      PRAWINPUTDEVICELIST pRawInputDeviceList, PUINT puiNumDevices,
      UINT cbSize);
  typedef UINT(WINAPI *tGetRawInputDeviceInfoW)(HANDLE hDevice, UINT uiCommand,
                                                LPVOID pData, PUINT pcbSize);

  tGetRawInputDeviceList pGetRawInputDeviceList;
  tGetRawInputDeviceInfoW pGetRawInputDeviceInfoW;

  static bool xinput_left_stick_motion(SHORT previous, SHORT current,
                                       di_event::axis_motion *motion_out) {
    di_event::axis_motion current_motion;

    if (current >= XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)
      current_motion = di_event::axis_positive;
    else if (current <= -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)
      current_motion = di_event::axis_negative;
    else
      current_motion = di_event::axis_center;

    if (previous != current) {
      *motion_out = current_motion;
      return true;
    }

    return false;
  }

  static bool xinput_right_stick_motion(SHORT previous, SHORT current,
                                        di_event::axis_motion *motion_out) {
    di_event::axis_motion current_motion;

    if (current >= XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE)
      current_motion = di_event::axis_positive;
    else if (current <= -XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE)
      current_motion = di_event::axis_negative;
    else
      current_motion = di_event::axis_center;

    if (previous != current) {
      *motion_out = current_motion;
      return true;
    }

    return false;
  }

  static int xinput_left_stick_deadzone(SHORT axis) {
    int iaxis = axis;
    int abs_axis = abs(((int)iaxis));
    if (abs_axis < XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)
      return 0;
    int sign_axis = iaxis >= 0 ? 1 : -1;
    return (abs_axis - XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) * 32767 /
           (32767 - XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) * sign_axis;
  }

  static int xinput_right_stick_deadzone(SHORT axis) {
    int iaxis = axis;
    int abs_axis = abs(((int)iaxis));
    if (abs_axis < XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE)
      return 0;
    int sign_axis = iaxis >= 0 ? 1 : -1;
    return (abs_axis - XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE) * 32767 /
           (32767 - XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE) * sign_axis;
  }

  static bool xinput_trigger_motion(BYTE previous, BYTE current,
                                    di_event::button_motion *motion_out) {
    if (current < previous) {
      *motion_out = di_event::button_up;
      return true;
    } else if (current >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD &&
               current > previous) {
      *motion_out = di_event::button_down;
      return true;
    }
    return false;
  }

  static int xinput_trigger_deadzone(BYTE trigger) {
    if (trigger < XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
      return 0;
    return (trigger - XINPUT_GAMEPAD_TRIGGER_THRESHOLD) * 255 /
           (255 - XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
  }

  bool is_xinput(const GUID *guid) {
    if (hXInput) {
      if (pGetRawInputDeviceList && pGetRawInputDeviceInfoW) {
        // The fast method, supported by XP and newer
        UINT nDevices, numDevices;
        RAWINPUTDEVICELIST *devices;
        UINT i;

        if (pGetRawInputDeviceList(NULL, &nDevices,
                                   sizeof(RAWINPUTDEVICELIST)) != 0)
          return false;
        if ((devices = (RAWINPUTDEVICELIST *)malloc(sizeof(RAWINPUTDEVICELIST) *
                                                    nDevices)) == NULL)
          return false;
        if ((numDevices = pGetRawInputDeviceList(
                 devices, &nDevices, sizeof(RAWINPUTDEVICELIST))) == (UINT)-1) {
          free(devices);
          return false;
        }
        for (i = 0; i < numDevices; ++i) {
          if (devices[i].dwType == RIM_TYPEHID) {
            RID_DEVICE_INFO rdi;
            UINT cbSize;

            cbSize = rdi.cbSize = sizeof(rdi);
            if (pGetRawInputDeviceInfoW(devices[i].hDevice, RIDI_DEVICEINFO,
                                        &rdi, &cbSize) >= 0) {
              if (MAKELONG(rdi.hid.dwVendorId, rdi.hid.dwProductId) ==
                  (LONG)guid->Data1) {
                wchar_t *name;
                UINT namelen = 0;
                UINT reslen;

                if (pGetRawInputDeviceInfoW(devices[i].hDevice, RIDI_DEVICENAME,
                                            NULL, &namelen) != 0) {
                  free(devices);
                  return false;
                }
                if ((name = (wchar_t *)malloc(sizeof(wchar_t) * namelen)) ==
                    NULL) {
                  free(devices);
                  return false;
                }
                if ((reslen = pGetRawInputDeviceInfoW(devices[i].hDevice,
                                                      RIDI_DEVICENAME, name,
                                                      &namelen)) < 0) {
                  free(name);
                  free(devices);
                  return false;
                }
                if (reslen > 0) {
                  bool is_xinput = wcsstr(name, L"IG_") != NULL;
                  free(name);
                  free(devices);
                  return is_xinput;
                }
              }
            }
          }
        }
        free(devices);
        return false;
      } else {
#define SAFE_RELEASE(p)                                                        \
  {                                                                            \
    if ((p)) {                                                                 \
      (p)->Release();                                                          \
      (p) = NULL;                                                              \
    }                                                                          \
  }
        IWbemLocator *wbemlocator = NULL;
        IEnumWbemClassObject *enumdevices = NULL;
        IWbemClassObject *devices[20] = {0};
        IWbemServices *wbemservices = NULL;
        BSTR namespce = NULL;
        BSTR deviceid = NULL;
        BSTR classname = NULL;
        DWORD returned = 0;
        bool is_xinput = false;
        UINT device = 0;
        VARIANT var;
        HRESULT hr;

        // Create WMI
        hr = CoCreateInstance(CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
                              IID_IWbemLocator, (LPVOID *)&wbemlocator);
        if (FAILED(hr) || wbemlocator == NULL)
          goto cleanup;

        if (NULL == (namespce = SysAllocString(OLESTR("\\\\.\\root\\cimv2"))))
          goto cleanup;
        if (NULL == (classname = SysAllocString(OLESTR("Win32_PNPEntity"))))
          goto cleanup;
        if (NULL == (deviceid = SysAllocString(OLESTR("DeviceID"))))
          goto cleanup;

        // Connect to WMI
        hr = wbemlocator->ConnectServer(namespce, NULL, NULL, 0, 0, NULL, NULL,
                                        &wbemservices);
        if (FAILED(hr) || wbemservices == NULL)
          goto cleanup;

        // Switch security level to IMPERSONATE.
        CoSetProxyBlanket(wbemservices, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                          NULL, RPC_C_AUTHN_LEVEL_CALL,
                          RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);

        hr = wbemservices->CreateInstanceEnum(classname, 0, NULL, &enumdevices);
        if (FAILED(hr) || enumdevices == NULL)
          goto cleanup;

        // Loop over all devices
        for (;;) {
          // Get 20 at a time.
          hr = enumdevices->Next(10000, _countof(devices), devices, &returned);
          if (FAILED(hr))
            goto cleanup;
          if (returned == 0)
            break;

          for (device = 0; device < returned; device++) {
            // For each device, get its device ID.
            hr = devices[device]->Get(deviceid, 0L, &var, NULL, NULL);
            if (SUCCEEDED(hr) && var.vt == VT_BSTR && var.bstrVal != NULL) {
              // Check if the device ID contains "IG_". If it does, then it's an
              // XInput device. This information cannot be found from
              // DirectInput.
              if (wcsstr(var.bstrVal, L"IG_")) {
                // If it does, then get the VID/PID from var.bstrVal.
                DWORD pid = 0, vid = 0;
                WCHAR *strvid = wcsstr(var.bstrVal, L"VID_");
                if (strvid && swscanf(strvid, L"VID_%4X", &vid) != 1)
                  vid = 0;
                WCHAR *strpid = wcsstr(var.bstrVal, L"PID_");
                if (strpid && swscanf(strpid, L"PID_%4X", &pid) != 1)
                  pid = 0;

                // Compare the VID/PID to the DInput device.
                DWORD vidpid = MAKELONG(vid, pid);
                if (vidpid == guid->Data1) {
                  is_xinput = true;
                  goto cleanup;
                }
              }
            }
            SAFE_RELEASE(devices[device]);
          }
        }
      cleanup:
        if (namespce)
          SysFreeString(namespce);
        if (deviceid)
          SysFreeString(deviceid);
        if (classname)
          SysFreeString(classname);
        for (device = 0; device < _countof(devices); ++device)
          SAFE_RELEASE(devices[device]);
        SAFE_RELEASE(enumdevices);
        SAFE_RELEASE(wbemlocator);
        SAFE_RELEASE(wbemservices);

        return is_xinput;
      }
    }

    return false;
  }

public:
  dinput_i() {
    lpdKeyboard = 0;
    lpdMouse = 0;
    focused = true;
    hXInput = NULL;
    // pXInputEnable = NULL;
    pXInputGetState = NULL;
  }

  virtual ~dinput_i() {
    std::vector<joy>::iterator it;
    for (it = joysticks.begin(); it < joysticks.end(); ++it) {
      it->device->Release();
      guids->remove(it->vidpid);
    }

    if (lpdMouse) {
      //  lpdMouse->Unacquire();
      //  lpdMouse->Release();
    }

    if (lpdKeyboard)
      lpdKeyboard->Release();

    if (hXInput)
      FreeLibrary(hXInput);
  }

  virtual const char *open(void *di8, void *hwnd, guid_container *guids) {
    lpDI = (LPDIRECTINPUT8)di8;
    HWND hWnd = (HWND)hwnd;

    this->guids = guids;

    if (lpDI->CreateDevice(GUID_SysKeyboard, &lpdKeyboard, 0) != DI_OK)
      return "Creating keyboard input";

    if (lpdKeyboard->SetDataFormat(&c_dfDIKeyboard) != DI_OK)
      return "Setting keyboard input format";

    if (lpdKeyboard->SetCooperativeLevel(hWnd, DISCL_NONEXCLUSIVE |
                                                   DISCL_FOREGROUND) != DI_OK)
      return "Setting keyboard cooperative level";

    if (lpDI->CreateDevice(GUID_SysMouse, &lpdMouse, 0) != DI_OK)
      return "creating mouse";
    if (lpdMouse->SetDataFormat(&c_dfDIMouse) != DI_OK)
      return "creating mouse input format";

    if (lpdMouse->SetCooperativeLevel(hWnd, DISCL_FOREGROUND |
                                                DISCL_NONEXCLUSIVE) != DI_OK)
      return "setting mouse cooperative level";

    lpdMouse->Acquire();
    DIPROPDWORD value;
    memset(&value, 0, sizeof(value));
    value.diph.dwSize = sizeof(value);
    value.diph.dwHeaderSize = sizeof(value.diph);
    // value.diph.dwObj = 0;
    value.diph.dwHow = DIPH_DEVICE;
    value.dwData = 128;
    if (lpdKeyboard->SetProperty(DIPROP_BUFFERSIZE, &value.diph) != DI_OK)
      return "Setting keyboard buffer size";

    hXInput = LoadLibrary(XINPUT_DLL);
    if (hXInput) {
      // pXInputEnable = ( tXInputEnable ) GetProcAddress( hXInput,
      // "XInputEnable" );
      pXInputGetState =
          (tXInputGetState)GetProcAddress(hXInput, "XInputGetState");

      for (unsigned i = 0; i < XUSER_MAX_COUNT; ++i)
        memset(&xinput_last_state[i], 0, sizeof(xinput_last_state[i]));

      HMODULE hUser = GetModuleHandle(_T("user32.dll"));
      pGetRawInputDeviceList = (tGetRawInputDeviceList)GetProcAddress(
          hUser, "GetRawInputDeviceList");
      pGetRawInputDeviceInfoW = (tGetRawInputDeviceInfoW)GetProcAddress(
          hUser, "GetRawInputDeviceInfoW");
    }

    if (lpDI->EnumDevices(DI8DEVCLASS_GAMECTRL, g_enum_callback, this,
                          DIEDFL_ATTACHEDONLY) != DI_OK)
      return "Enumerating joysticks";

    return 0;
  }

  virtual void poll_mouse() {
    HRESULT result;
    // Read the mouse device.
    DIMOUSESTATE mouse_state;
    memset(&mouse_state, 0, sizeof(mouse_state));
    if (FAILED(lpdMouse->GetDeviceState(sizeof(DIMOUSESTATE),
                                        (LPVOID)&mouse_state))) {
      lpdMouse->Acquire();
      if (FAILED(lpdMouse->GetDeviceState(sizeof(DIMOUSESTATE),
                                          (LPVOID)&mouse_state)))
        memset(&mouse_state, 0, sizeof(mouse_state));
    }
    mousie.x = mouse_state.lX;
    mousie.y = mouse_state.lY;
    mousie.lbutton = (mouse_state.rgbButtons[0] & 0x80) != 0;
    mousie.rbutton = (mouse_state.rgbButtons[1] & 0x80) != 0;
  }

  virtual void readmouse(int16_t &x, int16_t &y, bool &lbutton, bool &rbutton) {
    x = mousie.x;
    y = mousie.y;
    lbutton = mousie.lbutton;
    rbutton = mousie.rbutton;
  }

  virtual std::vector<di_event> read(bool poll_ = true) {
    std::vector<di_event> events;
    di_event e;

    HRESULT hr;

    if (focused) {
      e.type = di_event::ev_key;

      for (;;) {
        DIDEVICEOBJECTDATA buffer[32];
        DWORD n_events = 32;

        hr = lpdKeyboard->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), buffer,
                                        &n_events, 0);

        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
          lpdKeyboard->Acquire();
          break;
        }

        if (hr != DI_OK && hr != DI_BUFFEROVERFLOW)
          break;
        if (n_events == 0)
          break;

        for (unsigned i = 0; i < n_events; ++i) {
          DIDEVICEOBJECTDATA &od = buffer[i];
          e.key.which = od.dwOfs;

          if (od.dwData & 0x80)
            e.key.type = di_event::key_down;
          else
            e.key.type = di_event::key_up;

          events.push_back(e);
        }
      }
    }

    e.type = di_event::ev_joy;

    std::vector<joy>::iterator it;
    for (it = joysticks.begin(); it < joysticks.end(); ++it) {
      e.joy.serial = it->serial;
      if (poll_) {
        if (FAILED(it->device->Poll())) {
          if (FAILED(it->device->Acquire())) {
            continue;
          }
          it->device->Poll();
        }
      }

      for (;;) {
        DIDEVICEOBJECTDATA buffer[32];
        DWORD n_events = 32;

        hr = it->device->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), buffer,
                                       &n_events, 0);

        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
          it->device->Acquire();
          it->device->Poll();
          it->device->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), buffer,
                                    &n_events, 0);
        }

        if (hr != DI_OK && hr != DI_BUFFEROVERFLOW)
          break;
        if (n_events == 0)
          break;

        for (unsigned i = 0; i < n_events; ++i) {
          DIDEVICEOBJECTDATA &od = buffer[i];
          if (od.dwOfs >= DIJOFS_X && od.dwOfs <= DIJOFS_SLIDER(1)) {
            e.joy.type = di_event::joy_axis;
            e.joy.which = (od.dwOfs - DIJOFS_X) / (DIJOFS_Y - DIJOFS_X);
            e.joy.value = od.dwData;
            if (od.dwData < 0x8000 - 4096) {
              e.joy.axis = di_event::axis_negative;
            } else if (od.dwData > 0x8000 + 4096) {
              e.joy.axis = di_event::axis_positive;

            } else {
              e.joy.axis = di_event::axis_center;
            }

            events.push_back(e);
          } else if (od.dwOfs >= DIJOFS_POV(0) && od.dwOfs <= DIJOFS_POV(3)) {
            e.joy.type = di_event::joy_pov;
            e.joy.which =
                (od.dwOfs - DIJOFS_POV(0)) / (DIJOFS_POV(1) - DIJOFS_POV(0));
            e.joy.pov_angle = od.dwData;
            if (e.joy.pov_angle != ~0)
              e.joy.pov_angle /= DI_DEGREES;

            events.push_back(e);
          } else if (od.dwOfs >= DIJOFS_BUTTON(0) &&
                     od.dwOfs <= DIJOFS_BUTTON(31)) {
            e.joy.type = di_event::joy_button;
            e.joy.which = (od.dwOfs - DIJOFS_BUTTON(0)) /
                          (DIJOFS_BUTTON(1) - DIJOFS_BUTTON(0));
            if (od.dwData < 0) {
              continue;
            }
            if (od.dwData & 0x80)
              e.joy.button = di_event::button_down;
            else
              e.joy.button = di_event::button_up;

            events.push_back(e);
          }
        }
      }
    }

    if (pXInputGetState) {
      e.type = di_event::ev_xinput;

      XINPUT_STATE state;

      for (unsigned i = 0; i < XUSER_MAX_COUNT; ++i) {
        if (pXInputGetState(i, &state) != ERROR_SUCCESS)
          memset(&state, 0, sizeof(state));

        e.xinput.index = i;
        e.xinput.type = di_event::xinput_axis;

#define XINPUT_PUSH_AXIS(n, stick, v)                                          \
  e.xinput.which = n;                                                          \
  e.xinput.value = ((xinput_##stick##_stick_deadzone(state.Gamepad.##v)));     \
  if (xinput_##stick##_stick_motion(xinput_last_state[i].Gamepad.##v,          \
                                    state.Gamepad.##v, &e.xinput.axis))        \
  events.push_back(e)

        XINPUT_PUSH_AXIS(0, left, sThumbLX);
        XINPUT_PUSH_AXIS(1, left, sThumbLY);
        XINPUT_PUSH_AXIS(2, right, sThumbRX);
        XINPUT_PUSH_AXIS(3, right, sThumbRY);

#undef XINPUT_PUSH_AXIS

        e.xinput.type = di_event::xinput_trigger;

#define XINPUT_PUSH_TRIGGER(n, v)                                              \
  e.xinput.which = n;                                                          \
  e.xinput.value = xinput_trigger_deadzone(state.Gamepad.##v);                 \
  if (xinput_trigger_motion(xinput_last_state[i].Gamepad.##v,                  \
                            state.Gamepad.##v, &e.xinput.button))              \
  events.push_back(e)

        XINPUT_PUSH_TRIGGER(0, bLeftTrigger);
        XINPUT_PUSH_TRIGGER(1, bRightTrigger);

#undef XINPUT_PUSH_TRIGGER

        e.xinput.type = di_event::xinput_button;
        e.xinput.value = 0;

        for (unsigned button = 0, mask = 1; button < 14; ++button, mask <<= 1) {
          if (mask == 0x400)
            mask = 0x1000;
          if ((xinput_last_state[i].Gamepad.wButtons ^ state.Gamepad.wButtons) &
              mask) {
            e.xinput.which = button;
            if (state.Gamepad.wButtons & mask)
              e.xinput.button = di_event::button_down;
            else
              e.xinput.button = di_event::button_up;
            events.push_back(e);
          }
        }

        xinput_last_state[i] = state;
      }
    }

    return events;
  }

  virtual void set_focus(bool is_focused) {
    focused = is_focused;

    if (!is_focused && lpdKeyboard)
      lpdKeyboard->Unacquire();

    /* if ( pXInputEnable )
      pXInputEnable( is_focused ); */
  }

  virtual void refocus(void *hwnd) {
    if (lpdKeyboard->SetCooperativeLevel(
            (HWND)hwnd, DISCL_NONEXCLUSIVE | DISCL_FOREGROUND) == DI_OK)
      set_focus(true);
  }

  virtual const TCHAR *get_joystick_name(unsigned n) {
    std::vector<joy>::iterator it;
    for (it = joysticks.begin(); it < joysticks.end(); ++it) {
      if (it->serial == n) {
        return it->name.c_str();
      }
    }
    return _T("Unknown joystick");
  }

private:
  static BOOL PASCAL g_enum_callback(LPCDIDEVICEINSTANCEW lpDev,
                                     LPVOID lpContext) {
    dinput_i *p_this = (dinput_i *)lpContext;
    return p_this->enum_callback(lpDev);
  }

  BOOL enum_callback(LPCDIDEVICEINSTANCE lpDev) {
    if (is_xinput(&lpDev->guidProduct))
      return DIENUM_CONTINUE;

    LPDIRECTINPUTDEVICE8 lpdJoy;

    if (lpDI->CreateDevice(lpDev->guidInstance, &lpdJoy, 0) != DI_OK) {
      // OutputDebugString(_T("CreateDevice\n"));
      return DIENUM_CONTINUE;
    }

    if (lpdJoy->SetDataFormat(&c_dfDIJoystick) != DI_OK) {
      // OutputDebugString(_T("SetDataFormat\n"));
      lpdJoy->Release();
      return DIENUM_CONTINUE;
    }

    /*if ( lpdJoy->SetCooperativeLevel( hWnd, DISCL_NONEXCLUSIVE |
    DISCL_FOREGROUND ) != DI_OK )
    {
      lpdJoy->Release();
      return DIENUM_CONTINUE;
    }*/

    lpdJoy->EnumObjects(g_enum_objects_callback, this,
                        DIDFT_AXIS | DIDFT_BUTTON | DIDFT_POV);

    DIPROPDWORD value;
    memset(&value, 0, sizeof(value));
    value.diph.dwSize = sizeof(value);
    value.diph.dwHeaderSize = sizeof(value.diph);
    // value.diph.dwObj = 0;
    value.diph.dwHow = DIPH_DEVICE;
    value.dwData = 128;
    /*if (*/ lpdJoy->SetProperty(DIPROP_BUFFERSIZE, &value.diph); /*!= DI_OK )
    {
      OutputDebugString(_T("SetProperty DIPROP_BUFFERSIZE\n"));
      lpdJoy->Release();
      return DIENUM_CONTINUE;
    }*/

    {
      DIPROPRANGE diprg;
      diprg.diph.dwSize = sizeof(diprg);
      diprg.diph.dwHeaderSize = sizeof(diprg.diph);
      diprg.diph.dwHow = DIPH_BYOFFSET;

      diprg.lMin = -0x7fff;
      diprg.lMax = 0x7fff;

      DIPROPDWORD didz;
      didz.diph.dwSize = sizeof(didz);
      didz.diph.dwHeaderSize = sizeof(didz.diph);
      didz.diph.dwHow = DIPH_BYOFFSET;

      didz.dwData = 0x400;

      for (unsigned i = DIJOFS_X; i <= DIJOFS_SLIDER(1);
           i += (DIJOFS_Y - DIJOFS_X)) {
        diprg.diph.dwObj = i;
        didz.diph.dwObj = i;
        lpdJoy->SetProperty(DIPROP_RANGE, &diprg.diph);
        lpdJoy->SetProperty(DIPROP_DEADZONE, &didz.diph);
      }
    }

    if (lpdJoy->Acquire() != DI_OK) {
      // OutputDebugString(_T("Acquire\n"));
      lpdJoy->Release();
      return DIENUM_CONTINUE;
    }

    joysticks.push_back(joy(guids->add(lpDev->guidInstance.Data1),
                            lpDev->guidInstance.Data1, lpDev->tszInstanceName,
                            lpdJoy));

    return DIENUM_CONTINUE;
  }

  static BOOL PASCAL g_enum_objects_callback(LPCDIDEVICEOBJECTINSTANCEW lpDOI,
                                             LPVOID lpContext) {
    return DIENUM_CONTINUE;
  }
};

dinput *create_dinput() { return new dinput_i; }