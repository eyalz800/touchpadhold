#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <cstdio>
#include <iostream>
#include <memory>
#include <hidusage.h>
#include <hidsdi.h>
#include <stdexcept>
#include <system_error>
#include <chrono>

using namespace std::chrono_literals;

const wchar_t window_class_name[] = L"{DFFAB27A-9B47-43CB-947F-27D16D3D9635}";

static struct touchpad_state {
    using time_point = std::chrono::steady_clock::time_point;

    int coord_x = -1;
    int coord_y = -1;

    int last_coord_x = -1;
    int last_coord_y = -1;

    bool down = false;
    bool move = false;

    time_point last_input_time{};
    time_point last_input_timer_time{};
    time_point last_new_coord_time{};
} touchpad_state;

ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    try {
        UNREFERENCED_PARAMETER(hPrevInstance);
        UNREFERENCED_PARAMETER(lpCmdLine);

#ifdef _DEBUG
        AllocConsole();
        freopen("conin$", "r", stdin);
        freopen("conout$", "w", stdout);
        freopen("conout$", "w", stderr);
#endif

        EnableMouseInPointer(true);

        // Initialize global strings
        MyRegisterClass(hInstance);

        // Perform application initialization:
        InitInstance(hInstance, nCmdShow);

        MSG msg;

        // Main message loop:
        while (GetMessage(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        return static_cast<int>(msg.wParam);
    } catch (const std::exception & error) {
        std::cerr << "Error: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "Unknown error occurred.\n";
    }

    return 1;
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = nullptr;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = nullptr;
    wcex.lpszClassName = window_class_name;
    wcex.hIconSm = nullptr;

    return RegisterClassExW(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
   HWND hWnd = CreateWindowW(window_class_name, L"", WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);

   if (!hWnd) {
       throw std::system_error(GetLastError(),
                               std::system_category(),
                               "CreateWindow failed.");
   }

   RAWINPUTDEVICE raw_input_device[1];
   raw_input_device[0].usUsagePage = 0x0D;
   raw_input_device[0].dwFlags = RIDEV_INPUTSINK;
   raw_input_device[0].usUsage = 0x05;
   raw_input_device[0].hwndTarget = hWnd;

   if (RegisterRawInputDevices(raw_input_device, 1, sizeof(raw_input_device[0])) == FALSE) {
       throw std::system_error(GetLastError(), std::system_category(), "Register raw input failed");
   }

   return TRUE;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    case WM_INPUT: {
        UINT size{};
        auto raw_input_handle = reinterpret_cast<HRAWINPUT>(lParam);
        GetRawInputData(raw_input_handle, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER));

        auto lpb = std::make_unique<BYTE[]>(size);
        RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(lpb.get());

        GetRawInputData(
            raw_input_handle, RID_INPUT, lpb.get(), &size, sizeof(RAWINPUTHEADER));

        GetRawInputDeviceInfo(raw->header.hDevice, RIDI_PREPARSEDDATA, NULL, &size);
        auto preparsed_data_owner = std::make_unique<unsigned char[]>(size);
        PHIDP_PREPARSED_DATA preparsed_data = reinterpret_cast<PHIDP_PREPARSED_DATA>(preparsed_data_owner.get());
        GetRawInputDeviceInfo(raw->header.hDevice, RIDI_PREPARSEDDATA, preparsed_data, &size);

        NTSTATUS status{};
        HIDP_CAPS caps;
        status = HidP_GetCaps(preparsed_data, &caps);
        USHORT caps_length = caps.NumberInputValueCaps;
        auto value_caps = std::make_unique<HIDP_VALUE_CAPS[]>(caps_length);

        HidP_GetValueCaps(HidP_Input, value_caps.get(), &caps_length, preparsed_data);

        auto change = false;

        for (int i = 0; i < caps_length; i++) {
            ULONG value{};
            USHORT value_length = value_caps[i].BitSize * value_caps[i].ReportCount;
            HidP_GetUsageValue(HidP_Input,
                               value_caps[i].UsagePage,
                               0,
                               value_caps[i].NotRange.Usage,
                               &value,
                               preparsed_data,
                               (PCHAR)raw->data.hid.bRawData,
                               raw->data.hid.dwSizeHid);
            switch (value_caps[i].NotRange.Usage) {
            case 0x30:
                touchpad_state.last_coord_x = touchpad_state.coord_x;
                touchpad_state.coord_x = value;
                break;

            case 0x31:
                touchpad_state.last_coord_y = touchpad_state.coord_y;
                touchpad_state.coord_y = value;
                break;
            }

            if (touchpad_state.last_coord_x != touchpad_state.coord_x ||
                touchpad_state.last_coord_y != touchpad_state.coord_y
                ) {
                change = true;
            }
        }

        auto new_tap = false;
        auto input_time = std::chrono::steady_clock::now();
        if (input_time - touchpad_state.last_input_time > 50ms) {
            new_tap = true;
        }

        touchpad_state.last_input_time = input_time;

        if (new_tap) {
            touchpad_state.move = false;
            touchpad_state.last_new_coord_time = touchpad_state.last_input_time;
        } else if (change) {
            touchpad_state.last_new_coord_time = touchpad_state.last_input_time;
            touchpad_state.move = true;
            return 0;
        }

        if (!touchpad_state.down && !touchpad_state.move &&
            touchpad_state.last_input_time - touchpad_state.last_new_coord_time > 150ms
            ) {
            touchpad_state.down = true;
            mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            KillTimer(hwnd, 1337);
            touchpad_state.last_input_timer_time = touchpad_state.last_input_time;
            SetTimer(hwnd, 1337, 50, nullptr);
        }

        return 0;
    }
    case WM_TIMER: {
        if (touchpad_state.last_input_time == touchpad_state.last_input_timer_time) {
            mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            touchpad_state.down = false;
            KillTimer(hwnd, 1337);
        }
        touchpad_state.last_input_timer_time = touchpad_state.last_input_time;
    }

    default:
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
    return 0;
}
