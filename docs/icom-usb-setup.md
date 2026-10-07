# IC-7300 (original / MK1) and IC-9700 over USB

This adds local USB CI-V and USB sound-device connections to the existing Icom
backend. It uses AetherSDR's existing interface, DSP, recording, decoding,
memory, and transmit-control paths. Network connections and the separate
IC-7300MK2 profile remain available.

The original IC-7300 uses model ID / factory CI-V address `94`; the IC-9700
uses `A2`. The MK2 uses `B6`. A selected model is an identification hint;
capabilities are granted only after the connected radio answers identification.

## Radio and computer setup

1. Install Icom's USB driver appropriate to your operating system, then connect
   the radio's USB cable and turn the radio on. Close other programs using its
   CI-V port. When both radios are attached, identify each port and sound device
   individually; identical "USB Audio CODEC" names are common.
2. On the radio, set **CI-V USB Port** to **Unlink from [REMOTE]**, **CI-V USB
   Baud Rate** to **115200**, and **USB Serial Function** to **CI-V**. Select
   **AF** for USB audio output. Use a current original IC-7300 firmware revision
   (1.40 or later) for the documented scrolling scope functions.
3. Set **USB SEND**, **USB Keying (CW)**, and **USB Keying (RTTY)** to **OFF**.
   AetherSDR uses CI-V transmit commands. Opening the connection does not request
   transmit; RTS/DTR are not used for keying.
4. In AetherSDR's manual connection page choose **Icom (USB / network)**, then
   **USB**. Select that radio's CI-V port, receive sound device (recording/input),
   and transmit sound device (playback/output). These are the radio's USB sound
   devices; the app's normal microphone selection is still your PC microphone.
5. Choose **Auto** or the matching model. If you changed the radio's CI-V address,
   choose **Custom** and enter its actual hexadecimal address. Connect.
6. For PC audio transmission choose **PC Audio** through the existing mic-source
   control. The USB session routes modulation to USB, using the selected model's
   own DATA OFF MOD / DATA MOD and USB MOD LEVEL registers. Use the radio's
   **USB MOD Level** and the app's normal audio controls to set levels.

Saved USB selections participate in "Connect to last radio on start up" and
automatic reconnect. A missing selected device produces an error; it never
silently switches to a computer speaker or microphone. Memory imports use the
USB adapter's reported hardware serial number. If a driver does not expose one,
memory import is unavailable until a stable hardware identity can be supplied.

## Hardware and existing backend limits

- USB audio runs at 48 kHz with mono or stereo PCM. On the IC-9700 the left
  channel is used for the main receiver; sub-receiver audio is not mixed into
  decoders or recordings. Independently controlling and displaying both IC-9700
  receivers is not implemented by the existing Icom backend.
- The waterfall uses the radio's CI-V scope data, rather than USB I/Q. These
  radios cannot supply FlexRadio-style arbitrary slices or wideband USB I/Q.
- The IC-7300's internal tuner is exposed; the IC-9700 has no internal tuner.
  The original IC-7300 has TONE and TSQL controls, but no DTCS access selector.
- Original IC-7300 memories have ten-character names and a different record
  layout from MK2 memories. Split records remain visible but are not recalled
  as a single VFO, preserving their separate transmit frequency.
- Only one radio session is selected in the app at a time. Each USB connection
  carries its own port and audio selections into reconnect attempts.

## Validation status

This implementation is grounded in the original
[IC-7300 Full Manual, revision 12a](https://icomuk.co.uk/files/icom/PDF/advancedManuals/IC-7300_ENG_FM_12a.pdf),
chapter 19, and the
[IC-9700 CI-V Reference Guide, April 2021](https://www.icomfrance.com/uploads/files/produit/not-IC-9700_ENG_CI-V_3a-en.pdf).
New tests cover USB framing, echo filtering, PCM conversion, and distinct model
profiles and memory layouts. Existing CI-V, scope, meter, memory, audio, and
protocol tests are retained.

The complete Windows x64 application builds with MSVC and the production Qt
6.12.0 toolkit. All eight focused Icom tests pass against that build. A portable
payload audit resolves every dependency locally or through Windows, and a
native GUI smoke test confirms startup, the USB port/audio controls, and the
separate original IC-7300 and IC-9700 choices. Normal Windows release features,
including NR4 and GPU/ONNX/sherpa speech support, are included. Real-radio USB
verification remains outstanding; this is a development build for hardware
testing, not a claim of completed hardware certification.

For hardware validation, first confirm RX, tuning, front-panel reconciliation,
scope, and memory import on each radio. Then verify the existing transmit
controls, USB modulation, CW, tone functions, and TX audio drain using suitable
station test equipment. Confirm that connect, disconnect, and reconnect never
key either radio.
