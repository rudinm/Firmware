#include "i2c_helper.h"

#include <nuttx/config.h>
#include <nuttx/wqueue.h>
#include <nuttx/clock.h> 

#include <drivers/device/i2c.h>
#include <drivers/drv_hrt.h>

#include <sys/types.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>

#include <systemlib/perf_counter.h>
#include <systemlib/err.h>
#include <systemlib/systemlib.h>

#include <uORB/topics/i2c_button_status.h>

#include <board_config.h>

#include <drivers/drv_rgbled.h>

#define LISTENER_ADDR 0x20	/**< I2C adress of our button i2c controller */
#define I2C_BUTTON_COUNT 9

extern "C" void start_listener(int adress);
extern "C" void stop_listener(void);

class BUTTON_LISTENER : public device::I2C
{
public:
	BUTTON_LISTENER(int bus, int addr);
	virtual ~BUTTON_LISTENER();

	virtual int		init();
	virtual int		probe();
    void            start_listening();

private:
    bool			_running;
    bool			_should_run;
	work_s			_work;
    int			    _listening_interval;
    orb_advert_t    _cmd_pub; 

    struct i2c_button_s *_buttons;

    static void		listener_trampoline(void *arg);
    void			listen();
    int             get_buttons_state_from_r1();
    int             get_buttons_state_from_r2();
    void            init_buttons();
    void            check_button(struct i2c_button_s *button, int values);
    void            button_pressed(struct i2c_button_s *button, bool long_press);
};

/* for now, we only support one BUTTON_LISTENER */
namespace
{
BUTTON_LISTENER *_listener;
}


BUTTON_LISTENER::BUTTON_LISTENER(int bus, int addr) :
	I2C("buttons", "/dev/buttons", bus, addr, 100000),
    _running(false),
    _should_run(false),
    _listening_interval(USEC2TICK(100000))
{
	memset(&_work, 0, sizeof(_work));
}

BUTTON_LISTENER::~BUTTON_LISTENER()
{
    delete _buttons;
}

int
BUTTON_LISTENER::init()
{
	int ret;
	ret = I2C::init();

	if (ret != OK) {
		return ret;
	}

    init_buttons();

	return OK;
}

int
BUTTON_LISTENER::probe()
{
    uint8_t response[1] = {0};
    uint8_t requests[1] = {0x00};

    int ret = transfer(&requests[0], sizeof(requests), nullptr, 0);
    ret = transfer(nullptr, 0, response, sizeof(response)); 
    warnx("Result code is: %d, response: %x", ret, response[0]);

	return ret;
}

void
BUTTON_LISTENER::init_buttons()
{
    _buttons = new i2c_button_s[I2C_BUTTON_COUNT];
    int pin_id = 0;
    for (int i = 0; i < I2C_BUTTON_COUNT; i++) {
        if (pin_id == 8) {
            pin_id = 0;
        }
        _buttons[i] = { i, pin_id, false, false, 0};
        pin_id++;
    }

    _cmd_pub = -1;
}

void
BUTTON_LISTENER::listener_trampoline(void *arg)
{
	BUTTON_LISTENER *rgbl = reinterpret_cast<BUTTON_LISTENER *>(arg);

	rgbl->listen();
}

void
BUTTON_LISTENER::listen()
{
    if (!_should_run) {
		_running = false;
		return;
	}
    int mask_r1 = get_buttons_state_from_r1();

    for (int i = 0; i < I2C_BUTTON_COUNT - 1; i++) {
		check_button(&_buttons[i], mask_r1);
	}
    int mask_r2 = get_buttons_state_from_r2();
    for (int i = I2C_BUTTON_COUNT - 1; i < I2C_BUTTON_COUNT; i++) {
		check_button(&_buttons[i], mask_r2);
	}

    work_queue(LPWORK, &_work, (worker_t)&BUTTON_LISTENER::listener_trampoline, this, _listening_interval);
}

void
BUTTON_LISTENER::start_listening()
{
    _should_run = true;
    _running = true;
    work_queue(LPWORK, &_work, (worker_t)&BUTTON_LISTENER::listener_trampoline, this, _listening_interval);
}

int
BUTTON_LISTENER::get_buttons_state_from_r1()
{
    uint8_t response[1] = {0};
    uint8_t requests[1] = {0x00};

    int ret = transfer(&requests[0], sizeof(requests), nullptr, 0);
    ret = transfer(nullptr, 0, response, sizeof(response)); 

    if (ret == OK) {
        return response[0];
    }
    return -1;
}

int
BUTTON_LISTENER::get_buttons_state_from_r2()
{
    uint8_t response[1] = {0};
    uint8_t requests[2] = {0x00, 0x00};

    int ret = transfer(requests, sizeof(requests) * 2, nullptr, 0);
    ret = transfer(nullptr, 0, response, sizeof(response)); 

    if (ret == OK) {
        return response[0];
    }
    return -1;
}

void
BUTTON_LISTENER::check_button(struct i2c_button_s *button, int gpio_values)
{  
	if (!(gpio_values & (1 << button->register_pin))) {
		uint64_t now = hrt_absolute_time();
		float elapsed = (now - button->time_pressed) / 10000;

		if (button->button_pressed == false){
			button->button_pressed = true;
			button->time_pressed = now;
		} else if (button->button_pressed & !button->long_press & elapsed > 150) {
			warnx("long press button %d", button->pin + 1);
			button_pressed(button, true);
			button->long_press = true;
		}
	} else {
		if (button->button_pressed == true){
			if (!button->long_press)
			{
				warnx("short press button %d", button->pin + 1);
				button_pressed(button, false);
			}
			button->button_pressed = false;
			button->long_press = false;
		}
	}
};

void
BUTTON_LISTENER::button_pressed(struct i2c_button_s *button, bool long_press)
{
    if (_cmd_pub < 0) {
        _cmd_pub = orb_advertise(ORB_ID(i2c_button_status), button);
    } else {
        orb_publish(ORB_ID(i2c_button_status), _cmd_pub, button);
    }
}

void start_listener(int adress)
{
    int i2cdevice = -1;
	int rgbledadr = adress; /* 7bit */

    if (_listener != nullptr)
        errx(1, "already started");

    if (i2cdevice == -1) {
        // try the external bus first
        i2cdevice = PX4_I2C_BUS_EXPANSION;
        _listener = new BUTTON_LISTENER(i2cdevice, rgbledadr);
        int ret = _listener->init();
        if (_listener != nullptr && OK != ret) {
            delete _listener;
            _listener = nullptr;
            warnx("created but not inited with code: %d", ret);
        }

        if (_listener == nullptr) {
            // fall back to default bus
            i2cdevice = PX4_I2C_BUS_LED;
            warnx("Falled back to default bus");
        }
    }

    if (_listener == nullptr) {
        _listener = new BUTTON_LISTENER(i2cdevice, rgbledadr);
        warnx("creating device on default bus.");
        if (_listener == nullptr)
            errx(1, "new failed");
        int ret = _listener->init();
        if (OK != ret) {
            delete _listener;
            _listener = nullptr;
            errx(1, "init failed, code: %d", ret);
        }
    }
    if (_listener) {
        _listener->start_listening();
    }
}

void stop_listener()
{   
    delete _listener;
    _listener = nullptr;
}
