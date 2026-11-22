#pragma once

// esp-claw subsystem assembly (replaces upstream app_claw without its
// board/audio coupling). Call once after network + activation are ready.
void ClawPortStart();