#include "x64arm/midi.hpp"
#include <stdexcept>
namespace x64arm::midi {bool output_available(){return false;}std::unique_ptr<Output> open_output(std::shared_ptr<Playback>){throw std::runtime_error("DirectMusic synthesizer requires macOS");}}

namespace x64arm::midi {std::vector<InputDevice> input_devices(){return {};}std::unique_ptr<Input> open_input(std::uint32_t){throw std::runtime_error("MIDI input requires macOS");}}
