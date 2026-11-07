// report::capture - one picture of the game, on a key press. Only the game's own back buffer: never the desktop, never another window.
//
// REFramework hands a plugin the game's D3D12 device, swap chain and command queue (renderer_data) and calls it just before each Present
// (on_present, the game's render thread). The game has finished the frame then and the back buffer is in its PRESENT state. So:
//   press (game thread)        request(): one flag.
//   next Present               copy the back buffer to a readback buffer: a command list of our own on the swap chain's own queue, a fence.
//                              Allocations + recording, about 1 ms; nothing waits. Every other Present: one atomic load, no D3D call at all.
//   a thread of its own        waits for the fence (the game's thread never does), converts (pixels.hpp), downscales to 1080p at most, makes
//                              a JPEG through WIC, under kJpegMaxBytes, releases every D3D object. Nothing is ever written to disk.
// Any failure (a format not handled, a device lost, a fault: all under SEH) leaves no picture and the reason: the report goes without one.
// discard() forgets everything: a picture still on its way is dropped when it arrives.
//
// No REFramework and no game here: tests/report_capture_test.cpp runs all of it on a D3D12 device and swap chain of its own.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace report::capture {

enum class State { None, Waiting, Encoding, Ready, Failed };

void set_log(void (*sink)(const std::string&));

// Game thread: the next present() copies the frame. Whatever was there before is forgotten.
void request();
// The render thread, before Present (REFramework's on_present). `queue` must be the swap chain's. Nothing to do: one atomic load.
void present(void* device, void* swap_chain, void* queue);
// Forget it all (the menu went away).
void discard();

State state();
// The copy was queued on the GPU, or given up: what the screen shows from now on is not in the picture.
inline bool settled(State s) { return s != State::Waiting && s != State::None; }
// State::Ready: the JPEG, moved out (nothing stays here). Otherwise empty.
std::vector<uint8_t> take();
// State::Failed: why, in English, for the report's notes and the journal
std::string why();

// The encoder (WIC), for the tests too: bgr = h rows of `pitch` bytes. The first quality in the list that fits max_bytes wins, then half the
// size. false: not even that. COM must be up on this thread.
bool encode_jpeg(const uint8_t* bgr, int w, int h, size_t pitch, size_t max_bytes, std::vector<uint8_t>* out, std::string* why);

}  // namespace report::capture
