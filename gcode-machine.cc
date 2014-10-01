/* -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * (c) h.zeller@acm.org. Free Software. GNU Public License v3.0 and above
 */

#include "machine.h"

#include <assert.h>
#include <stdio.h>
#include <math.h>

#include "tape.h"
#include "board.h"

#include "pnp-config.h"

// Hovering while transporting a component.
// TODO: calculate that to not knock over already placed components.
#define Z_HOVERING 10

// Components are a bit higher as they are resting on some card-board. Let's
// assume some value here.
// Ultimately, we want the placement operation be a bit spring-loaded.
#define TAPE_THICK 0.0

// All templates should be in a separate file somewhere so that we don't
// have to compile.

// Multiplication to get 360 degrees mapped to one turn. This is specific to
// our stepper motor. TODO: make configurable
#define ANGLE_FACTOR (50.34965 / 360)

// Speeds in mm/s
#define TO_TAPE_SPEED 1000  // moving needle to tape
#define TO_BOARD_SPEED 100  // moving component from tape to board

#define Z_DISPENSING_ABOVE 0.3      // Above board when dispensing
#define Z_HOVER_ABOVE 2             // Above board when moving around
#define Z_SEPARATE_DROPLET_ABOVE 5  // Above board right after dispensing.

// param: moving needle up.
static const char *const gcode_preamble = R"(
G28 X0 Y0  ; Home (x/y) - needle over free space
G28 Z0     ; Now it is safe to home z
G21        ; set to mm
T1         ; Use E1 extruder, our 'A' axis.
M302       ; cold extrusion override - because it is not actually an extruder.
G90        ; Use absolute positions in general.
G92 E0     ; 'home' E axis

G1 Z%.1f E0 ; Move needle out of way
)";

// param: name, move-speed, x, y, zup, zdown, a, zup
static const char *const gcode_pick = R"(
;; -- Pick %s
G0 F%d X%.3f Y%.3f Z%.3f E%.3f ; Move over component to pick.
G1 Z%-6.2f   F4000 ; move down on tape.
G4           ; flush buffer
M42 P6 S255  ; turn on suckage
G1 Z%-6.3f   ; Move up a bit for travelling
)";

// param: name, place-speed, x, y, zup, a, zdown, zup
static const char *const gcode_place = R"(
;; -- Place %s
G0 F%d X%.3f Y%.3f Z%.3f E%.3f ; Move component to place on board.
G1 Z%-6.3f F4000 ; move down over board thickness.
G4            ; flush buffer.
M42 P6 S0     ; turn off suckage
G4            ; flush buffer.
M42 P8 S255   ; blow
G4 P40        ; .. for 40ms
M42 P8 S0     ; done.
G1 Z%-6.2f    ; Move up
)";

// move to new position, above board.
// param: component-name, pad-name, x, y, hover-z
static const char *const gcode_dispense_move = R"(
;; -- component %s, pad %s
G0 X%.3f Y%.3f Z%.3f   ; move there.)";

// Dispense paste.
// param: z-dispense-height, wait-time-ms, area, z-separate-droplet
static const char *const gcode_dispense_paste = R"(
G1 Z%.2f ; Go down to dispense
M106      ; switch on fan (=solenoid)
G4 P%-5.1f ; Wait time dependent on area %.2f mm^2
M107      ; switch off solenoid
G1 Z%.2f ; high above to have paste separated
)";

static const char *const gcode_finish = R"(
G28 X0 Y0  ; Home x/y, but leave z clear
M84        ; stop motors
)";

GCodeMachine::GCodeMachine(float init_ms, float area_ms)
    : init_ms_(init_ms), area_ms_(area_ms), config_(NULL) {}

bool GCodeMachine::Init(const PnPConfig *config,
                        const std::string &init_comment,
                        const Dimension& dim) {
    config_ = config;
    if (config_ == NULL) {
        fprintf(stderr, "Need configuration\n");
        return false;
    }
    fprintf(stderr, "Board-thickness = %.1fmm\n",
            config_->board.top - config_->bed_level);
    printf("; %s\n", init_comment.c_str());
    float highest_tape = config_->board.top;
    for (const auto &t : config_->tape_for_component) {
        highest_tape = std::max(highest_tape, t.second->height());
    }
    printf(gcode_preamble, highest_tape + 10);
    return true;
}

void GCodeMachine::PickPart(const Part &part, const Tape *tape) {
    if (tape == NULL) return;
    float px, py;
    if (!tape->GetPos(&px, &py)) {
        fprintf(stderr, "We are out of components for %s %s\n",
                part.footprint.c_str(), part.value.c_str());
        return;
    }

    const float board_thick = config_->board.top - config_->bed_level;
    const float travel_height = tape->height() + board_thick + Z_HOVERING;
    const std::string print_name = part.component_name + " ("
        + part.footprint + "@" + part.value + ")";

    // param: name, x, y, zdown, a, zup
    printf(gcode_pick,
           print_name.c_str(),
           60 * TO_TAPE_SPEED,
           px, py, tape->height() + Z_HOVERING,         // component pos.
           ANGLE_FACTOR * fmod(tape->angle(), 360.0),   // pickup angle
           tape->height(),                              // down to component
           travel_height);                              // up for travel.
}

void GCodeMachine::PlacePart(const Part &part, const Tape *tape) {
    if (tape == NULL) return;
    const float board_thick = config_->board.top - config_->bed_level;
    const float travel_height = tape->height() + board_thick + Z_HOVERING;
    const std::string print_name = part.component_name + " ("
        + part.footprint + "@" + part.value + ")";

    // param: name, x, y, zup, a, zdown, zup
    printf(gcode_place,
           print_name.c_str(),
           60 * TO_BOARD_SPEED,
           part.pos.x + config_->board.origin.x,
           part.pos.y + config_->board.origin.y,
           travel_height,
           ANGLE_FACTOR * fmod(part.angle - tape->angle() + 360, 360.0),
           tape->height() + board_thick - TAPE_THICK,
           travel_height);
}

 void GCodeMachine::Dispense(const Part &part, const Pad &pad) {
     // TODO: so this part looks like we shouldn't have to do it here.
     const float angle = 2 * M_PI * part.angle / 360.0;
     const float part_x = config_->board.origin.x + part.pos.x;
     const float part_y = config_->board.origin.y + part.pos.y;
     const float area = pad.size.w * pad.size.h;
     const float pad_x = pad.pos.x;
     const float pad_y = pad.pos.y;
     const float x = part_x + pad_x * cos(angle) - pad_y * sin(angle);
     const float y = part_y + pad_x * sin(angle) + pad_y * cos(angle);
     printf(gcode_dispense_move,
            part.component_name.c_str(), pad.name.c_str(),
            x, y, config_->board.top + Z_HOVER_ABOVE);
     printf(gcode_dispense_paste,
            config_->board.top + Z_DISPENSING_ABOVE,
            init_ms_ + area * area_ms_, area,
            config_->board.top + Z_SEPARATE_DROPLET_ABOVE);
}

void GCodeMachine::Finish() {
    printf(gcode_finish);
}
