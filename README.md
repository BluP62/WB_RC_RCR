# WB_RC_RCR
EVSE Wallbox Remote Control for Ripple Control Receiver Input

The Smart-WB (https://github.com/CurtRod/SimpleEVSE-WiFi) is based on EVSE which allows to be controlled by your grid operator to reduce the total energy consumption to 4,2kW. 
The EVSE has 2 pins to be shortend and the wallbox will automatically reduce the power flow from (e.g. 11kW) to 4,2kw in order to reduce power consumption from the grid via ripple control reciever input (Rundsteuerempänger)
Especially in Germany with the rollout of Smartmeter/gateways and control units, you can make use of this. 
When allowing the grid provider to reduce the energy down to 4,2kw for controllable units (https://www.gesetze-im-internet.de/enwg_2005/__14a.html) and make use of Modul 1 & Modul 3 you can save money.

Actually you can simply connect the 2 pins of the EVSe controller to the control unit (NC) of your smartmeter, but in my case the WB is approx. 60m away from the Smartmeter and I did not run additional cable with 2 wires for that. But the EVSE is connected my WLAN and can be controlled from there.
The idea was to "remote control" the 2 pins to be shortend via WLAN.
I used a shelly 1 switch to act as the "remote" switch for the RCR.
The output from the pysical RCR relais (NO) is read from an ESP32 and whenever it is activated, the ESP32 sends a HTTP request to the shelly to switch on or off according to the input pins from the ESP32/RCR relais.

In addition to that, the current status (IP, ON-OFF, voltage, currents, RCR active) of the EVSE is displayed on an OLED and 3 LEDS show a "system alive" blue LED flash, a glowing (or steady ON) green LED for the status of the EVSE WB and a flashing red LED when the RCR signal is active.
