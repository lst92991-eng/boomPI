#ifndef BOOMPI_APPLICATION_MANUAL_SINGLE_TURN_H_
#define BOOMPI_APPLICATION_MANUAL_SINGLE_TURN_H_

#include "boompi/event/status.h"

namespace boompi::application {

// Runs the deliberately single-threaded P1 milestone from bounded environment
// configuration: stream one fixed capture, receive one response, and play it.
// The caller owns process/runtime startup and shutdown.
Status RunManualSingleTurnFromEnvironment();

}  // namespace boompi::application

#endif  // BOOMPI_APPLICATION_MANUAL_SINGLE_TURN_H_
