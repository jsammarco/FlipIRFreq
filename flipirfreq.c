#include <furi.h>
#include <furi_hal_infrared.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <storage/storage.h>
#include <toolbox/saved_struct.h>
#include <stdlib.h>
#include <stdio.h>

#define FLIPIRFREQ_DEFAULT_FREQUENCY 38000U
#define FLIPIRFREQ_DEFAULT_DUTY_CYCLE 33U
#define FLIPIRFREQ_DEFAULT_BURST_MS 250U
#define FLIPIRFREQ_TX_CHUNK_US 25000U
#define FLIPIRFREQ_UI_TICK_HZ 8U
#define FLIPIRFREQ_SETTINGS_PATH APP_DATA_PATH("flipirfreq.settings")
#define FLIPIRFREQ_SETTINGS_MAGIC 0x49
#define FLIPIRFREQ_SETTINGS_VERSION 1U
#define FLIPIRFREQ_MIN_DUTY_CYCLE 1U
#define FLIPIRFREQ_MAX_DUTY_CYCLE 99U
#define FLIPIRFREQ_MIN_BURST_MS 1U
#define FLIPIRFREQ_MAX_BURST_MS 5000U

typedef enum {
    FlipIRFreqFieldFrequency,
    FlipIRFreqFieldDutyCycle,
    FlipIRFreqFieldBurst,
    FlipIRFreqFieldMode,
    FlipIRFreqFieldOutput,
    FlipIRFreqFieldSend,
    FlipIRFreqFieldCount,
} FlipIRFreqField;

typedef enum {
    FlipIRFreqOutputAuto,
    FlipIRFreqOutputInternal,
    FlipIRFreqOutputExternal,
    FlipIRFreqOutputCount,
} FlipIRFreqOutput;

typedef enum {
    FlipIRFreqModeBurst,
    FlipIRFreqModeContinuous,
    FlipIRFreqModeCount,
} FlipIRFreqMode;

typedef enum {
    FlipIRFreqEventTypeInput,
    FlipIRFreqEventTypeTick,
    FlipIRFreqEventTypeTxFinished,
} FlipIRFreqEventType;

typedef struct {
    FlipIRFreqEventType type;
    InputEvent input;
} FlipIRFreqEvent;

typedef struct {
    uint32_t frequency;
    uint8_t duty_cycle;
    uint16_t burst_ms;
    uint8_t output_mode;
    uint8_t tx_mode;
} FlipIRFreqSettings;

typedef struct {
    bool continuous;
    uint32_t remaining_us;
    uint32_t chunk_us;
} FlipIRFreqTxContext;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* event_queue;
    FuriTimer* ui_timer;

    uint32_t frequency;
    uint8_t duty_cycle;
    uint16_t burst_ms;
    FlipIRFreqField selected_field;
    FlipIRFreqOutput output_mode;
    FlipIRFreqMode tx_mode;

    bool running;
    bool transmitting;
    bool settings_dirty;
    uint32_t last_settings_change_tick;
    uint32_t tx_started_tick;
    uint8_t tx_anim_phase;
    FlipIRFreqTxContext tx_context;
    char status[40];
} FlipIRFreqApp;

static uint32_t flipirfreq_clamp_i32_to_u32(int32_t value, uint32_t min, uint32_t max) {
    if(value < (int32_t)min) return min;
    if((uint32_t)value > max) return max;
    return (uint32_t)value;
}

static uint8_t flipirfreq_clamp_i32_to_u8(int32_t value, uint8_t min, uint8_t max) {
    if(value < min) return min;
    if(value > max) return max;
    return (uint8_t)value;
}

static const char* flipirfreq_output_short_label(FuriHalInfraredTxPin output) {
    switch(output) {
    case FuriHalInfraredTxPinInternal:
        return "INT";
    case FuriHalInfraredTxPinExtPA7:
        return "EXT";
    default:
        return "?";
    }
}

static const char* flipirfreq_output_mode_label(FlipIRFreqOutput mode) {
    switch(mode) {
    case FlipIRFreqOutputAuto:
        return "AUTO";
    case FlipIRFreqOutputInternal:
        return "INT";
    case FlipIRFreqOutputExternal:
        return "EXT";
    default:
        return "?";
    }
}

static const char* flipirfreq_mode_label(FlipIRFreqMode mode) {
    switch(mode) {
    case FlipIRFreqModeBurst:
        return "BURST";
    case FlipIRFreqModeContinuous:
        return "CONT";
    default:
        return "?";
    }
}

static FuriHalInfraredTxPin flipirfreq_resolve_output(FlipIRFreqApp* app) {
    if(app->output_mode == FlipIRFreqOutputInternal) {
        return FuriHalInfraredTxPinInternal;
    } else if(app->output_mode == FlipIRFreqOutputExternal) {
        return FuriHalInfraredTxPinExtPA7;
    } else {
        return furi_hal_infrared_detect_tx_output();
    }
}

static uint32_t flipirfreq_frequency_step(bool coarse) {
    return coarse ? 1000U : 100U;
}

static uint16_t flipirfreq_burst_step(bool coarse) {
    return coarse ? 100U : 10U;
}

static void flipirfreq_set_status(FlipIRFreqApp* app, const char* text) {
    snprintf(app->status, sizeof(app->status), "%s", text);
}

static void flipirfreq_mark_settings_dirty(FlipIRFreqApp* app) {
    app->settings_dirty = true;
    app->last_settings_change_tick = furi_get_tick();
}

static void flipirfreq_load_defaults(FlipIRFreqApp* app) {
    app->frequency = FLIPIRFREQ_DEFAULT_FREQUENCY;
    app->duty_cycle = FLIPIRFREQ_DEFAULT_DUTY_CYCLE;
    app->burst_ms = FLIPIRFREQ_DEFAULT_BURST_MS;
    app->output_mode = FlipIRFreqOutputAuto;
    app->tx_mode = FlipIRFreqModeBurst;
}

static void flipirfreq_save_settings(FlipIRFreqApp* app) {
    FlipIRFreqSettings settings = {
        .frequency = app->frequency,
        .duty_cycle = app->duty_cycle,
        .burst_ms = app->burst_ms,
        .output_mode = app->output_mode,
        .tx_mode = app->tx_mode,
    };

    if(saved_struct_save(
           FLIPIRFREQ_SETTINGS_PATH,
           &settings,
           sizeof(settings),
           FLIPIRFREQ_SETTINGS_MAGIC,
           FLIPIRFREQ_SETTINGS_VERSION)) {
        app->settings_dirty = false;
    }
}

static void flipirfreq_load_settings(FlipIRFreqApp* app) {
    FlipIRFreqSettings settings = {0};
    flipirfreq_load_defaults(app);

    if(saved_struct_load(
           FLIPIRFREQ_SETTINGS_PATH,
           &settings,
           sizeof(settings),
           FLIPIRFREQ_SETTINGS_MAGIC,
           FLIPIRFREQ_SETTINGS_VERSION)) {
        app->frequency = flipirfreq_clamp_i32_to_u32(
            settings.frequency, INFRARED_MIN_FREQUENCY, INFRARED_MAX_FREQUENCY);
        app->duty_cycle = flipirfreq_clamp_i32_to_u8(
            settings.duty_cycle, FLIPIRFREQ_MIN_DUTY_CYCLE, FLIPIRFREQ_MAX_DUTY_CYCLE);
        app->burst_ms =
            flipirfreq_clamp_i32_to_u32(settings.burst_ms, FLIPIRFREQ_MIN_BURST_MS, FLIPIRFREQ_MAX_BURST_MS);
        app->output_mode = (settings.output_mode < FlipIRFreqOutputCount) ?
                               (FlipIRFreqOutput)settings.output_mode :
                               FlipIRFreqOutputAuto;
        app->tx_mode = (settings.tx_mode < FlipIRFreqModeCount) ?
                           (FlipIRFreqMode)settings.tx_mode :
                           FlipIRFreqModeBurst;
    }
}

static FuriHalInfraredTxGetDataState
    flipirfreq_tx_data_callback(void* context, uint32_t* duration, bool* level) {
    FlipIRFreqTxContext* tx_context = context;
    *level = true;

    if(tx_context->continuous) {
        *duration = tx_context->chunk_us;
        return FuriHalInfraredTxGetDataStateDone;
    }

    if(tx_context->remaining_us > tx_context->chunk_us) {
        tx_context->remaining_us -= tx_context->chunk_us;
        *duration = tx_context->chunk_us;
        return FuriHalInfraredTxGetDataStateDone;
    }

    *duration = tx_context->remaining_us;
    tx_context->remaining_us = 0;
    return FuriHalInfraredTxGetDataStateLastDone;
}

static void flipirfreq_tx_finished_callback(void* context) {
    FlipIRFreqApp* app = context;
    FlipIRFreqEvent event = {.type = FlipIRFreqEventTypeTxFinished};
    furi_message_queue_put(app->event_queue, &event, 0);
}

static void flipirfreq_stop_transmit(FlipIRFreqApp* app, bool interrupted) {
    if(!app->transmitting) {
        return;
    }

    furi_hal_infrared_async_tx_stop();
    furi_hal_infrared_async_tx_set_signal_sent_isr_callback(NULL, NULL);
    furi_hal_infrared_async_tx_set_data_isr_callback(NULL, NULL);
    app->transmitting = false;
    app->tx_anim_phase = 0;

    if(interrupted) {
        flipirfreq_set_status(app, "Transmission stopped");
    } else if(app->tx_mode == FlipIRFreqModeContinuous) {
        flipirfreq_set_status(app, "Continuous transmit ended");
    } else {
        flipirfreq_set_status(app, "Burst transmit complete");
    }
}

static void flipirfreq_start_transmit(FlipIRFreqApp* app) {
    if(furi_hal_infrared_is_busy()) {
        flipirfreq_set_status(app, "IR subsystem busy");
        return;
    }

    const FuriHalInfraredTxPin output = flipirfreq_resolve_output(app);
    app->tx_context = (FlipIRFreqTxContext){
        .continuous = (app->tx_mode == FlipIRFreqModeContinuous),
        .remaining_us = (uint32_t)app->burst_ms * 1000U,
        .chunk_us = FLIPIRFREQ_TX_CHUNK_US,
    };

    furi_hal_infrared_set_tx_output(output);
    furi_hal_infrared_async_tx_set_data_isr_callback(flipirfreq_tx_data_callback, &app->tx_context);
    furi_hal_infrared_async_tx_set_signal_sent_isr_callback(
        (app->tx_mode == FlipIRFreqModeBurst) ? flipirfreq_tx_finished_callback : NULL,
        app);
    furi_hal_infrared_async_tx_start(app->frequency, (float)app->duty_cycle / 100.0f);

    app->transmitting = true;
    app->tx_started_tick = furi_get_tick();
    app->tx_anim_phase = 0;
    app->selected_field = FlipIRFreqFieldSend;

    snprintf(app->status, sizeof(app->status), "Broadcasting on %s", flipirfreq_output_short_label(output));
}

static void flipirfreq_adjust_field(FlipIRFreqApp* app, bool increase, bool coarse) {
    const int32_t direction = increase ? 1 : -1;

    switch(app->selected_field) {
    case FlipIRFreqFieldFrequency: {
        int32_t next = (int32_t)app->frequency + (int32_t)flipirfreq_frequency_step(coarse) * direction;
        app->frequency =
            flipirfreq_clamp_i32_to_u32(next, INFRARED_MIN_FREQUENCY, INFRARED_MAX_FREQUENCY);
        break;
    }
    case FlipIRFreqFieldDutyCycle: {
        int32_t next = (int32_t)app->duty_cycle + (coarse ? 5 : 1) * direction;
        app->duty_cycle =
            flipirfreq_clamp_i32_to_u8(next, FLIPIRFREQ_MIN_DUTY_CYCLE, FLIPIRFREQ_MAX_DUTY_CYCLE);
        break;
    }
    case FlipIRFreqFieldBurst: {
        int32_t next = (int32_t)app->burst_ms + (int32_t)flipirfreq_burst_step(coarse) * direction;
        app->burst_ms =
            flipirfreq_clamp_i32_to_u32(next, FLIPIRFREQ_MIN_BURST_MS, FLIPIRFREQ_MAX_BURST_MS);
        break;
    }
    case FlipIRFreqFieldMode: {
        int32_t next = (int32_t)app->tx_mode + direction;
        if(next < 0) {
            next = FlipIRFreqModeCount - 1;
        } else if(next >= FlipIRFreqModeCount) {
            next = 0;
        }
        app->tx_mode = next;
        break;
    }
    case FlipIRFreqFieldOutput: {
        int32_t next = (int32_t)app->output_mode + direction;
        if(next < 0) {
            next = FlipIRFreqOutputCount - 1;
        } else if(next >= FlipIRFreqOutputCount) {
            next = 0;
        }
        app->output_mode = next;
        break;
    }
    default:
        break;
    }
}

static const char* flipirfreq_tx_indicator(uint8_t phase) {
    static const char* frames[] = {"TX", "TX.", "TX..", "TX..."};
    return frames[phase % COUNT_OF(frames)];
}

static void flipirfreq_draw_row(
    Canvas* canvas,
    int32_t y,
    bool selected,
    const char* label,
    const char* value) {
    if(selected) {
        canvas_draw_box(canvas, 0, y - 8, 128, 10);
        canvas_set_color(canvas, ColorWhite);
    } else {
        canvas_set_color(canvas, ColorBlack);
    }

    canvas_draw_str(canvas, 3, y, label);
    canvas_draw_str_aligned(canvas, 125, y, AlignRight, AlignBottom, value);

    if(selected) {
        canvas_set_color(canvas, ColorBlack);
    }
}

static void flipirfreq_draw_callback(Canvas* canvas, void* context) {
    FlipIRFreqApp* app = context;
    char value[32];

    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "FlipIRFreq");
    if(app->transmitting) {
        canvas_draw_str_aligned(
            canvas, 125, 10, AlignRight, AlignBottom, flipirfreq_tx_indicator(app->tx_anim_phase));
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 20, app->status);

    snprintf(value, sizeof(value), "%lu Hz", (unsigned long)app->frequency);
    flipirfreq_draw_row(
        canvas, 24, app->selected_field == FlipIRFreqFieldFrequency, "Freq", value);

    snprintf(value, sizeof(value), "%u%%", app->duty_cycle);
    flipirfreq_draw_row(
        canvas, 32, app->selected_field == FlipIRFreqFieldDutyCycle, "Duty", value);

    snprintf(value, sizeof(value), "%u ms", (unsigned int)app->burst_ms);
    flipirfreq_draw_row(canvas, 40, app->selected_field == FlipIRFreqFieldBurst, "Burst", value);

    snprintf(value, sizeof(value), "%s", flipirfreq_mode_label(app->tx_mode));
    flipirfreq_draw_row(canvas, 48, app->selected_field == FlipIRFreqFieldMode, "Mode", value);

    if(app->output_mode == FlipIRFreqOutputAuto) {
        snprintf(
            value,
            sizeof(value),
            "AUTO>%s",
            flipirfreq_output_short_label(flipirfreq_resolve_output(app)));
    } else {
        snprintf(value, sizeof(value), "%s", flipirfreq_output_mode_label(app->output_mode));
    }
    flipirfreq_draw_row(canvas, 56, app->selected_field == FlipIRFreqFieldOutput, "Out", value);

    snprintf(
        value,
        sizeof(value),
        "%s",
        app->transmitting ? "STOP" : (app->tx_mode == FlipIRFreqModeContinuous ? "START" : "SEND"));
    flipirfreq_draw_row(canvas, 63, app->selected_field == FlipIRFreqFieldSend, "Send", value);
}

static void flipirfreq_input_callback(InputEvent* input_event, void* context) {
    FlipIRFreqApp* app = context;
    FlipIRFreqEvent event = {.type = FlipIRFreqEventTypeInput, .input = *input_event};
    furi_message_queue_put(app->event_queue, &event, 0);
}

static void flipirfreq_handle_input(FlipIRFreqApp* app, const InputEvent* input) {
    const bool pressed = (input->type == InputTypeShort) || (input->type == InputTypeRepeat);
    const bool coarse = input->type == InputTypeRepeat;

    if(input->key == InputKeyBack && input->type == InputTypeShort) {
        if(app->transmitting) {
            flipirfreq_stop_transmit(app, true);
            return;
        }
        app->running = false;
        return;
    }

    if(app->transmitting) {
        if((input->key == InputKeyOk) && (input->type == InputTypeShort)) {
            flipirfreq_stop_transmit(app, true);
        }
        return;
    }

    if(!pressed && input->key != InputKeyOk) {
        return;
    }

    switch(input->key) {
    case InputKeyUp:
        if(pressed) {
            if(app->selected_field == 0) {
                app->selected_field = FlipIRFreqFieldCount - 1;
            } else {
                app->selected_field--;
            }
        }
        break;
    case InputKeyDown:
        if(pressed) {
            app->selected_field = (app->selected_field + 1) % FlipIRFreqFieldCount;
        }
        break;
    case InputKeyLeft:
        if(pressed) {
            flipirfreq_adjust_field(app, false, coarse);
            if(app->selected_field != FlipIRFreqFieldSend) {
                flipirfreq_mark_settings_dirty(app);
            }
        }
        break;
    case InputKeyRight:
        if(pressed) {
            flipirfreq_adjust_field(app, true, coarse);
            if(app->selected_field != FlipIRFreqFieldSend) {
                flipirfreq_mark_settings_dirty(app);
            }
        }
        break;
    case InputKeyOk:
        if(input->type == InputTypeShort) {
            if(app->selected_field == FlipIRFreqFieldSend) {
                if(app->transmitting) {
                    flipirfreq_stop_transmit(app, true);
                } else {
                    flipirfreq_start_transmit(app);
                }
            }
        }
        break;
    default:
        break;
    }
}

static void flipirfreq_tick_callback(void* context) {
    FlipIRFreqApp* app = context;
    FlipIRFreqEvent event = {.type = FlipIRFreqEventTypeTick};
    furi_message_queue_put(app->event_queue, &event, 0);
}

static void flipirfreq_handle_tick(FlipIRFreqApp* app) {
    if(app->transmitting) {
        app->tx_anim_phase = (app->tx_anim_phase + 1) & 0x03;
    }

    if(app->settings_dirty &&
       ((furi_get_tick() - app->last_settings_change_tick) >= (furi_kernel_get_tick_frequency() / 2))) {
        flipirfreq_save_settings(app);
    }
}

static FlipIRFreqApp* flipirfreq_app_alloc(void) {
    FlipIRFreqApp* app = malloc(sizeof(FlipIRFreqApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->view_port = view_port_alloc();
    app->event_queue = furi_message_queue_alloc(8, sizeof(FlipIRFreqEvent));
    app->ui_timer = furi_timer_alloc(flipirfreq_tick_callback, FuriTimerTypePeriodic, app);

    flipirfreq_load_settings(app);
    app->selected_field = FlipIRFreqFieldFrequency;
    app->running = true;
    app->transmitting = false;
    app->settings_dirty = false;
    app->last_settings_change_tick = 0;
    app->tx_started_tick = 0;
    app->tx_anim_phase = 0;
    flipirfreq_set_status(app, "");

    view_port_draw_callback_set(app->view_port, flipirfreq_draw_callback, app);
    view_port_input_callback_set(app->view_port, flipirfreq_input_callback, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);
    furi_timer_start(app->ui_timer, furi_kernel_get_tick_frequency() / FLIPIRFREQ_UI_TICK_HZ);

    return app;
}

static void flipirfreq_app_free(FlipIRFreqApp* app) {
    if(!app) return;

    if(app->transmitting) {
        flipirfreq_stop_transmit(app, true);
    }
    if(app->settings_dirty) {
        flipirfreq_save_settings(app);
    }
    furi_timer_stop(app->ui_timer);
    furi_timer_free(app->ui_timer);
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_message_queue_free(app->event_queue);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t flipirfreq_app(void* p) {
    UNUSED(p);

    FlipIRFreqApp* app = flipirfreq_app_alloc();
    FlipIRFreqEvent event;

    view_port_update(app->view_port);

    while(app->running) {
        if(furi_message_queue_get(app->event_queue, &event, FuriWaitForever) == FuriStatusOk) {
            if(event.type == FlipIRFreqEventTypeInput) {
                flipirfreq_handle_input(app, &event.input);
            } else if(event.type == FlipIRFreqEventTypeTick) {
                flipirfreq_handle_tick(app);
            } else if(event.type == FlipIRFreqEventTypeTxFinished) {
                if(app->tx_mode == FlipIRFreqModeBurst) {
                    flipirfreq_stop_transmit(app, false);
                }
            }
            view_port_update(app->view_port);
        }
    }

    flipirfreq_app_free(app);
    return 0;
}
