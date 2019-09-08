/*
    simarduboy - Arduboy simulator
    Copyright (C) 2019 Guus Sliepen <guus@perpendicular-dimensions.org>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <iostream>
#include <stdexcept>
#include <cstring>
#include <thread>
#include <unistd.h>

#include <avr_ioport.h>
#include <sim_avr.h>
#include <sim_elf.h>
#include <sim_hex.h>

#include <SDL.h>

#include "ssd1306_virt.h"

static bool running = true;

static void run_core(avr_t *avr) {
	while(running) {
		avr_run(avr);
	}
}

static void draw_display(ssd1306_t &display, SDL_Texture *texture, SDL_Renderer *renderer) {
	// Convert the 128x64 pixel mono image to a 32 bpp texture
	uint32_t pixels[128 * 64];
	uint32_t *p = pixels;

	for (int y = 0; y < 64; y++) {
		for (int x = 0; x < 128; x++) {
			*p++ = (display.vram[y / 8][x] >> (y & 7)) & 1 ? 0xffffff : 0;
		}
	}

	SDL_UpdateTexture(texture, NULL, pixels, 128 * 4);
	SDL_RenderCopy(renderer, texture, nullptr, nullptr);
	SDL_RenderPresent(renderer);
}

static uint8_t speaker_value;
static const uint16_t nsamples = 1024;
static const uint32_t audio_frequency = 8000;
static uint16_t samples[nsamples];
static uint16_t cur_sample = 0;
static uint16_t out_sample = 0;
static avr_cycle_count_t next = 16000000 / audio_frequency;

// Called whenever the speaker output pins change
static void speaker_hook(struct avr_irq_t * irq, uint32_t value, void * param) {
	speaker_value = value;
}

// Should be called audio_frequency times per second on average
static avr_cycle_count_t speaker_timer(avr_t * avr, avr_cycle_count_t when, void * param) {
	samples[cur_sample++] = speaker_value * 0x4000;
	cur_sample %= nsamples;
	return when + next;
}

// Called by SDL whenever it needs another chunk of audio
static void audio_callback(void *userdata, uint8_t *stream, int len) {
	memcpy(stream, samples + out_sample, len);
	out_sample += len / sizeof(*samples);
	out_sample %= nsamples;
}

int main(int argc, char *argv[]) {
	if (argc <= 1) {
		std::cerr << "Usage: " << argv[0] << " firmware.elf|firmware.hex\n";
		return 1;
	}

	// Initialize the AVR core

	avr_t *avr = avr_make_mcu_by_name("atmega32u4");
	if(!avr)
		throw std::runtime_error("Could not create AVR instance");

	avr_init(avr);

	if (strstr(argv[1], ".elf")) {
		elf_firmware_t program;
		elf_read_firmware(argv[1], &program);
		program.frequency = 16000000;
		avr_load_firmware(avr, &program);
	} else {
		uint32_t dsize;
		uint32_t start;
		uint8_t *data = read_ihex_file(argv[1], &dsize, &start);
		avr->frequency = 16000000;
		avr_loadcode(avr, data, dsize, start);
	}

	// Initialize the SSD1306 display controller emulator
	ssd1306_t display;
	ssd1306_init(avr, &display, 128, 64);

	ssd1306_wiring_t wiring = {{'D', 6}, {'D', 4}, {'D', 7}};
	ssd1306_connect (&display, &wiring);

	// Initialize the speaker emulator
	avr_irq_register_notify(avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('C'), 7), speaker_hook, nullptr);
	avr_irq_register_notify(avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('C'), 6), speaker_hook, nullptr);
	avr_cycle_timer_register_usec(avr, 125, speaker_timer, nullptr);

        // Initialize SDL

        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0)
        	throw std::runtime_error("Could not initialize SDL");

	auto screen = SDL_CreateRGBSurface(0, 128, 64, 32, 0xff0000, 0xff00, 0xff, 0xff000000);
	if (!screen)
		throw std::runtime_error("Could not create surface");

	auto window = SDL_CreateWindow("simarduboy", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 256, 128, SDL_WINDOW_RESIZABLE);
	if (!window)
		throw std::runtime_error("Could not create window");

	auto renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
	if (!renderer)
		throw std::runtime_error("Could not create renderer");
	
	auto texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 128, 64);
	if (!texture)
		throw std::runtime_error("Could not create texture");
	
	SDL_AudioSpec want = {}, have;
	want.freq = audio_frequency;
	want.format = AUDIO_S16;
	want.channels = 1;
	want.samples = nsamples;
	want.callback = audio_callback;

	if (SDL_OpenAudio(&want, &have) < 0 || want.freq != have.freq)
		throw std::runtime_error("Could not open audio device");

	SDL_ShowCursor(SDL_DISABLE);

	// Run the emulation in the background

	std::thread core_thread(run_core, avr);
	SDL_PauseAudio(0);

	// Run the event loop

	enum Button {
		BUTTON_UP,
		BUTTON_DOWN,
		BUTTON_LEFT,
		BUTTON_RIGHT,
		BUTTON_A,
		BUTTON_B,
		BUTTON_COUNT
	};

	avr_irq_t *irq[BUTTON_COUNT];
	for(auto &&i: irq)
		i = avr_alloc_irq(&avr->irq_pool, 0, 1, nullptr);

	avr_connect_irq(irq[BUTTON_UP], avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('F'), 7));
	avr_connect_irq(irq[BUTTON_DOWN], avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('F'), 4));
	avr_connect_irq(irq[BUTTON_LEFT], avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('F'), 5));
	avr_connect_irq(irq[BUTTON_RIGHT], avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('F'), 6));
	avr_connect_irq(irq[BUTTON_A], avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('E'), 6));
	avr_connect_irq(irq[BUTTON_B], avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('B'), 4));

	while(running) {
		SDL_Event event;

		while (SDL_PollEvent(&event)) {
			switch(event.type) {
			case SDL_QUIT:
				running = false;
				break;
			case SDL_KEYDOWN:
			case SDL_KEYUP: {
				uint8_t value = event.type == SDL_KEYDOWN ? 0 : 1;
				switch(event.key.keysym.scancode) {
				case SDL_SCANCODE_UP:
					avr_raise_irq(irq[BUTTON_UP], value);
					break;
				case SDL_SCANCODE_DOWN:
					avr_raise_irq(irq[BUTTON_DOWN], value);
					break;
				case SDL_SCANCODE_LEFT:
					avr_raise_irq(irq[BUTTON_LEFT], value);
					break;
				case SDL_SCANCODE_RIGHT:
					avr_raise_irq(irq[BUTTON_RIGHT], value);
					break;
				case SDL_SCANCODE_A:
					avr_raise_irq(irq[BUTTON_A], value);
					break;
				case SDL_SCANCODE_S:
					avr_raise_irq(irq[BUTTON_B], value);
					break;
				default:
					break;
				}
				break;
			}
			default:
				break;
			}
		}

		// Draw the display

		draw_display(display, texture, renderer);
	}

	// Clean up

	core_thread.join();
}
