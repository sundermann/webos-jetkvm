#include "cursor.hpp"
#include "file.hpp"
#include "input.hpp"
#include "input_device.hpp"

#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

// Existing TV input-manager ABI, supplied by libim.so.1.
extern "C" int IM_RequestToCheckInput();
namespace stream {
namespace {
class WebosInputDevice final : public InputDevice {
    UniqueFd fd_;
    Cursor cursor_;

  public:
    WebosInputDevice() {
        fd_ = UniqueFd(open("/dev/uinput", O_WRONLY | O_CLOEXEC));
        if (fd_.get() < 0) {
            throw std::runtime_error("Cannot open /dev/uinput (root required)");
        }
        {
            auto set = [this](unsigned long request, int value) {
                if (ioctl(fd_.get(), request, value) < 0) {
                    throw std::runtime_error("Cannot configure uinput");
                }
            };
            set(UI_SET_EVBIT, EV_KEY);
            set(UI_SET_EVBIT, EV_ABS);
            set(UI_SET_EVBIT, EV_REL);
            set(UI_SET_EVBIT, EV_SYN);
            for (const auto key : Input::key_codes()) {
                set(UI_SET_KEYBIT, static_cast<int>(key));
            }
            set(UI_SET_KEYBIT, BTN_LEFT);
            set(UI_SET_KEYBIT, BTN_MIDDLE);
            set(UI_SET_ABSBIT, ABS_X);
            set(UI_SET_ABSBIT, ABS_Y);
            set(UI_SET_RELBIT, REL_WHEEL);
            uinput_user_dev device{};
            std::strncpy(device.name, "webos WebRTC remote", sizeof(device.name) - 1);
            device.id.bustype = BUS_USB;
            device.id.version = 4;
            device.absmax[ABS_X] = 1919;
            device.absmax[ABS_Y] = 1079;
            if (write(fd_.get(), &device, sizeof(device)) != sizeof(device) ||
                ioctl(fd_.get(), UI_DEV_CREATE) < 0) {
                throw std::runtime_error("Cannot create uinput remote");
            }
        }
        if (IM_RequestToCheckInput() < 0) {
            throw std::runtime_error("LG input manager rejected device");
        }
    }
    void write_event(unsigned type, unsigned code, int value) override {
        input_event event{};
        gettimeofday(&event.time, nullptr);
        event.type = static_cast<__u16>(type);
        event.code = static_cast<__u16>(code);
        event.value = value;
        if (write(fd_.get(), &event, sizeof(event)) != sizeof(event)) {
            throw std::runtime_error("uinput write failed");
        }
    }
    void hide_cursor() override {
        cursor_.hide();
    }
};
} // namespace
std::unique_ptr<InputDevice> make_webos_input_device() {
    return std::make_unique<WebosInputDevice>();
}
} // namespace stream
