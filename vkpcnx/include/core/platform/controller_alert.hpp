#pragma once

// Gets the player's attention through the controllers: the HOME-button
// notification LED (Joy-Con R, Pro Controller; hid:sys) blinks and every pad
// gives a light rumble on each flash. No-ops on platforms other than the Switch.
namespace vkpcnx::platform {

// Blinks 4 times over ~2.5 s, then the LED goes dark and the rumble stops by
// themselves. A new call restarts the alert.
void alertControllers();

// Stops an alert still in progress
void clearControllerAlert();

} // namespace vkpcnx::platform
