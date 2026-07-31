#ifndef BOOMPI_VOICE_CLIENT_H_
#define BOOMPI_VOICE_CLIENT_H_

#include <csignal>

#include "boompi/config/voice_client_config.h"

namespace boompi::voice {

Status RunVoiceClient(const config::VoiceClientConfig& config,
                      const volatile std::sig_atomic_t* stop);

}  // namespace boompi::voice

#endif  // BOOMPI_VOICE_CLIENT_H_
