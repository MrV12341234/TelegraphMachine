This Machine uses little Telegraph DIY kits, adding an Arduino Nano and 
OLED screen to send and receive telegrams.

Operation:
Either side can begin sending a message. Once one side begins, the other now must wait.
When the sender finishes (waits 7 seconds), the message will send. 
The system pauses until the receiver presses their button. Then everything is reset. 

Wiring:

OLED:
GND → GND 
VCC → 5v board power 
SDA → A4 
SCK/SCL → A5

Button: one side ground, other side goes to  D2 
Buzzer: One side ground, another side goes to D6

Board A D11 → Board B D10 
Board B D11 → Board A D10 
Board A GND → Board B GND
Board A 5V → Board B 5V
(this connection eliminates the need to power the second nano with a power source)
