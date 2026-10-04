#include "FirstPersonController.hpp"

// The behaviour lives in the header so other scripts, such as MusicSwitch and ToneButton, can hold
// a relay::Ref<FirstPersonController> and read the player's view.
RELAY_BEHAVIOUR(FirstPersonController)
