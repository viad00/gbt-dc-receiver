/*
 * display_ui.c
 *
 *  Created on: Dec 10, 2025
 *      Author: vladislav utkin
 * LICENSE: MIT
 */
#include "display_ui.h"

#include "main.h"
#include "gbt_27930_bms.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define SSD1306_I2C_ADDR        (0x3Cu << 1)
#define SSD1306_WIDTH           128u
#define SSD1306_HEIGHT          64u
#define SSD1306_FB_SIZE         (SSD1306_WIDTH * SSD1306_HEIGHT / 8u)
#define UI_DEBOUNCE_MS          20u
#define UI_REFRESH_MS           250u
#define OLED_RETRY_INIT_MS      2000u
#define UI_BUTTON_ACTIVE_STATE   GPIO_PIN_SET

typedef enum {
  UI_BTN_UP = 0,
  UI_BTN_DOWN,
  UI_BTN_SHARP,
  UI_BTN_STAR,
  UI_BTN_COUNT
} UiButton;

typedef enum {
  UI_SCREEN_MAIN = 0,
  UI_SCREEN_PARAMS,
  UI_SCREEN_FLASH
} UiScreen;

typedef struct {
  GPIO_TypeDef *port;
  uint16_t pin;
} UiButtonState;

typedef struct {
  GbtRuntimeField field;
  const char *label;
  const char *unit;
  uint16_t divisor;
  uint8_t decimals;
  int32_t step;
} UiFieldMeta;

static uint8_t oled_fb[SSD1306_FB_SIZE];
static UiScreen ui_screen = UI_SCREEN_MAIN;
static uint8_t ui_main_selected = 0u;
static uint8_t ui_param_selected = 0u;
static bool ui_param_edit = false;
static uint8_t ui_flash_selected = 0u;
static bool ui_dirty = true;
static bool oled_ready = false;
static uint32_t ui_last_refresh = 0u;
static uint32_t oled_last_init_try = 0u;
static volatile uint32_t ui_button_events = 0u;
static volatile uint32_t ui_last_irq_at[UI_BTN_COUNT] = {0u};

static UiButtonState ui_buttons[UI_BTN_COUNT] = {
  {BTN_UP_GPIO_Port, BTN_UP_Pin},
  {BTN_DOWN_GPIO_Port, BTN_DOWN_Pin},
  {BTN_SHARP_GPIO_Port, BTN_SHARP_Pin},
  {BTN_STAR_GPIO_Port, BTN_STAR_Pin},
};

static const UiFieldMeta ui_quick_fields[] = {
  {GBT27930_RUNTIME_DEMAND_VOLTAGE, "DV", "V", 10, 1, 10},
  {GBT27930_RUNTIME_DEMAND_CURRENT, "DI", "A", 10, 1, 10},
  {GBT27930_RUNTIME_MODE, "MD", "", 1, 0, 1},
};

static const UiFieldMeta ui_ccs_fields[] = {
  {GBT27930_RUNTIME_PACK_VOLTAGE, "PV", "V", 10, 1, 1},
  {GBT27930_RUNTIME_PACK_CURRENT, "PI", "A", 10, 1, 1},
  {GBT27930_RUNTIME_PERMIT_CHARGE, "PC", "", 1, 0, 1},
};

static const UiFieldMeta ui_all_fields[] = {
  {GBT27930_RUNTIME_DEMAND_VOLTAGE, "DV", "V", 10, 1, 5},
  {GBT27930_RUNTIME_DEMAND_CURRENT, "DI", "A", 10, 1, 5},
  {GBT27930_RUNTIME_MODE, "MD", "", 1, 0, 1},
  {GBT27930_RUNTIME_PACK_VOLTAGE, "PV", "V", 10, 1, 1},
  {GBT27930_RUNTIME_PACK_CURRENT, "PI", "A", 10, 1, 1},
  {GBT27930_RUNTIME_TAIL_SWITCH_VOLTAGE, "SV", "V", 10, 1, 1},
  {GBT27930_RUNTIME_TAIL_SWITCH_CURRENT, "SC", "A", 10, 1, 1},
  {GBT27930_RUNTIME_TAIL_VOLTAGE, "TV", "V", 10, 1, 1},
  {GBT27930_RUNTIME_TAIL_CURRENT, "TC", "A", 10, 1, 1},
  {GBT27930_RUNTIME_MAX_CELL_MV, "CM", "V", 1000, 2, 10},
  {GBT27930_RUNTIME_MAX_CELL_GROUP, "CG", "", 1, 0, 1},
  {GBT27930_RUNTIME_SOC_PERCENT, "SO", "P", 1, 0, 1},
  {GBT27930_RUNTIME_REMAINING_MIN, "RM", "M", 1, 0, 1},
  {GBT27930_RUNTIME_MAX_CELL_INDEX, "CX", "", 1, 0, 1},
  {GBT27930_RUNTIME_TEMP_MAX_C, "TX", "C", 1, 0, 1},
  {GBT27930_RUNTIME_TEMP_MAX_INDEX, "TI", "", 1, 0, 1},
  {GBT27930_RUNTIME_TEMP_MIN_C, "TN", "C", 1, 0, 1},
  {GBT27930_RUNTIME_TEMP_MIN_INDEX, "NI", "", 1, 0, 1},
  {GBT27930_RUNTIME_PERMIT_CHARGE, "PC", "", 1, 0, 1},
};

static const char *UI_GetStateText(GbtChargeState state)
{
  switch (state) {
    case GBT27930_CHARGE_STATE_WAIT_CHM: return "WAIT";
    case GBT27930_CHARGE_STATE_SEND_BHM: return "BHM";
    case GBT27930_CHARGE_STATE_SEND_BRM: return "BRM";
    case GBT27930_CHARGE_STATE_SEND_BCP: return "BCP";
    case GBT27930_CHARGE_STATE_SEND_BRO: return "BRO";
    case GBT27930_CHARGE_STATE_CHARGING: return "CHG";
    case GBT27930_CHARGE_STATE_END: return "END";
    default: return "UNK";
  }
}

static const char *UI_GetModeText(int32_t mode)
{
  return (mode == 0x02) ? "CC" : "CV";
}

static void UI_FormatFixed(char *out, size_t out_len, int32_t value, uint16_t divisor, uint8_t decimals)
{
  int32_t whole = value / divisor;
  int32_t frac = value % divisor;
  if (frac < 0) {
    frac = -frac;
  }

  if (decimals == 0u) {
    (void)snprintf(out, out_len, "%ld", (long)whole);
  } else if (decimals == 1u) {
    (void)snprintf(out, out_len, "%ld.%01ld", (long)whole, (long)(frac / (divisor / 10u)));
  } else if (decimals == 2u) {
    (void)snprintf(out, out_len, "%ld.%02ld", (long)whole, (long)(frac / (divisor / 100u)));
  } else {
    (void)snprintf(out, out_len, "%ld", (long)value);
  }
}

static const uint8_t *UI_GetGlyph(char c)
{
  static const uint8_t space[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
  static const uint8_t hash[5]  = {0x14, 0x7F, 0x14, 0x7F, 0x14};
  static const uint8_t star[5]  = {0x08, 0x2A, 0x1C, 0x2A, 0x08};
  static const uint8_t dash[5]  = {0x08, 0x08, 0x08, 0x08, 0x08};
  static const uint8_t caret[5] = {0x04, 0x02, 0x01, 0x02, 0x04};
  static const uint8_t greater[5] = {0x20, 0x10, 0x08, 0x10, 0x20};

  static const uint8_t digits[10][5] = {
      {0x3E, 0x51, 0x49, 0x45, 0x3E},
      {0x00, 0x42, 0x7F, 0x40, 0x00},
      {0x42, 0x61, 0x51, 0x49, 0x46},
      {0x21, 0x41, 0x45, 0x4B, 0x31},
      {0x18, 0x14, 0x12, 0x7F, 0x10},
      {0x27, 0x45, 0x45, 0x45, 0x39},
      {0x3C, 0x4A, 0x49, 0x49, 0x30},
      {0x01, 0x71, 0x09, 0x05, 0x03},
      {0x36, 0x49, 0x49, 0x49, 0x36},
      {0x06, 0x49, 0x49, 0x29, 0x1E},
  };

  static const uint8_t letters[26][5] = {
      {0x7E, 0x11, 0x11, 0x11, 0x7E},
      {0x7F, 0x49, 0x49, 0x49, 0x36},
      {0x3E, 0x41, 0x41, 0x41, 0x22},
      {0x7F, 0x41, 0x41, 0x22, 0x1C},
      {0x7F, 0x49, 0x49, 0x49, 0x41},
      {0x7F, 0x09, 0x09, 0x09, 0x01},
      {0x3E, 0x41, 0x49, 0x49, 0x7A},
      {0x7F, 0x08, 0x08, 0x08, 0x7F},
      {0x00, 0x41, 0x7F, 0x41, 0x00},
      {0x20, 0x40, 0x41, 0x3F, 0x01},
      {0x7F, 0x08, 0x14, 0x22, 0x41},
      {0x7F, 0x40, 0x40, 0x40, 0x40},
      {0x7F, 0x02, 0x0C, 0x02, 0x7F},
      {0x7F, 0x04, 0x08, 0x10, 0x7F},
      {0x3E, 0x41, 0x41, 0x41, 0x3E},
      {0x7F, 0x09, 0x09, 0x09, 0x06},
      {0x3E, 0x41, 0x51, 0x21, 0x5E},
      {0x7F, 0x09, 0x19, 0x29, 0x46},
      {0x46, 0x49, 0x49, 0x49, 0x31},
      {0x01, 0x01, 0x7F, 0x01, 0x01},
      {0x3F, 0x40, 0x40, 0x40, 0x3F},
      {0x1F, 0x20, 0x40, 0x20, 0x1F},
      {0x3F, 0x40, 0x38, 0x40, 0x3F},
      {0x63, 0x14, 0x08, 0x14, 0x63},
      {0x07, 0x08, 0x70, 0x08, 0x07},
      {0x61, 0x51, 0x49, 0x45, 0x43},
  };

  if (c == ' ') return space;
  if (c == '#') return hash;
  if (c == '*') return star;
  if (c == '-') return dash;
  if (c == '^') return caret;
  if (c == '>') return greater;
  if ((c >= '0') && (c <= '9')) return digits[(uint8_t)(c - '0')];
  if ((c >= 'A') && (c <= 'Z')) return letters[(uint8_t)(c - 'A')];
  return space;
}

static HAL_StatusTypeDef SSD1306_WriteCommand(uint8_t cmd)
{
  uint8_t frame[2] = {0x00u, cmd};
  return HAL_I2C_Master_Transmit(&hi2c2, SSD1306_I2C_ADDR, frame, sizeof(frame), 20u);
}

static HAL_StatusTypeDef SSD1306_WriteData(const uint8_t *data, uint16_t len)
{
  uint8_t frame[17];
  frame[0] = 0x40u;

  while (len > 0u) {
    uint16_t chunk = (len > 16u) ? 16u : len;
    memcpy(&frame[1], data, chunk);
    if (HAL_I2C_Master_Transmit(&hi2c2, SSD1306_I2C_ADDR, frame, (uint16_t)(chunk + 1u), 20u) != HAL_OK) {
      return HAL_ERROR;
    }
    data += chunk;
    len -= chunk;
  }

  return HAL_OK;
}

static bool SSD1306_Init(void)
{
  const uint8_t init_seq[] = {
      0xAE, 0x20, 0x00, 0xB0, 0xC8,
      0x00, 0x10, 0x40, 0x81, 0x7F,
      0xA1, 0xA6, 0xA8, 0x3F, 0xA4,
      0xD3, 0x00, 0xD5, 0x80, 0xD9,
      0xF1, 0xDA, 0x12, 0xDB, 0x40,
      0x8D, 0x14, 0xAF
  };

  for (uint32_t i = 0; i < (uint32_t)(sizeof(init_seq) / sizeof(init_seq[0])); i++) {
    if (SSD1306_WriteCommand(init_seq[i]) != HAL_OK) {
      return false;
    }
  }

  return true;
}

static void SSD1306_Clear(void)
{
  memset(oled_fb, 0x00, sizeof(oled_fb));
}

static void SSD1306_DrawPixel(uint8_t x, uint8_t y, bool on)
{
  if ((x >= SSD1306_WIDTH) || (y >= SSD1306_HEIGHT)) {
    return;
  }

  uint16_t index = (uint16_t)x + ((uint16_t)(y / 8u) * SSD1306_WIDTH);
  uint8_t mask = (uint8_t)(1u << (y % 8u));
  if (on) {
    oled_fb[index] |= mask;
  } else {
    oled_fb[index] &= (uint8_t)~mask;
  }
}

static void SSD1306_DrawRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
  if ((w == 0u) || (h == 0u)) {
    return;
  }

  for (uint8_t i = 0; i < w; i++) {
    SSD1306_DrawPixel((uint8_t)(x + i), y, true);
    SSD1306_DrawPixel((uint8_t)(x + i), (uint8_t)(y + h - 1u), true);
  }
  for (uint8_t i = 0; i < h; i++) {
    SSD1306_DrawPixel(x, (uint8_t)(y + i), true);
    SSD1306_DrawPixel((uint8_t)(x + w - 1u), (uint8_t)(y + i), true);
  }
}

static void SSD1306_DrawChar(uint8_t x, uint8_t y, char c)
{
  const uint8_t *glyph = UI_GetGlyph(c);
  for (uint8_t col = 0; col < 5u; col++) {
    uint8_t bits = glyph[col];
    for (uint8_t row = 0; row < 7u; row++) {
      SSD1306_DrawPixel((uint8_t)(x + col), (uint8_t)(y + row), (bits & (1u << row)) != 0u);
    }
  }
}

static void SSD1306_DrawText(uint8_t x, uint8_t y, const char *text)
{
  while ((*text != '\0') && (x <= (SSD1306_WIDTH - 6u))) {
    SSD1306_DrawChar(x, y, *text++);
    x = (uint8_t)(x + 6u);
  }
}

static bool SSD1306_Update(void)
{
  for (uint8_t page = 0; page < 8u; page++) {
    if (SSD1306_WriteCommand((uint8_t)(0xB0u + page)) != HAL_OK) return false;
    if (SSD1306_WriteCommand(0x00u) != HAL_OK) return false;
    if (SSD1306_WriteCommand(0x10u) != HAL_OK) return false;
    if (SSD1306_WriteData(&oled_fb[page * SSD1306_WIDTH], SSD1306_WIDTH) != HAL_OK) return false;
  }
  return true;
}

static void UI_DrawFieldLine(uint8_t x, uint8_t y, char marker, const UiFieldMeta *meta)
{
  char value[18];
  char line[24];
  int32_t current = GbtGetRuntimeField(meta->field);

  if (meta->field == GBT27930_RUNTIME_MODE) {
    (void)snprintf(value, sizeof(value), "%s", UI_GetModeText(current));
  } else if (meta->field == GBT27930_RUNTIME_PERMIT_CHARGE) {
    (void)snprintf(value, sizeof(value), "%s", (current != 0) ? "ON" : "OFF");
  } else {
    char formatted[16];
    UI_FormatFixed(formatted, sizeof(formatted), current, meta->divisor, meta->decimals);
    if (meta->unit[0] != '\0') {
      (void)snprintf(value, sizeof(value), "%s%s", formatted, meta->unit);
    } else {
      (void)snprintf(value, sizeof(value), "%s", formatted);
    }
  }

  (void)snprintf(line, sizeof(line), "%c%s %s", marker, meta->label, value);
  SSD1306_DrawText(x, y, line);
}

static void UI_RenderMain(void)
{
  char line[24];
  const char *state = UI_GetStateText(GbtGetChargeState());

  SSD1306_Clear();
  SSD1306_DrawRect(0u, 0u, SSD1306_WIDTH, SSD1306_HEIGHT);
  SSD1306_DrawText(8u, 4u, "STATE");
  (void)snprintf(line, sizeof(line), "ST %s", state);
  SSD1306_DrawText(52u, 4u, line);

  for (uint8_t i = 0; i < 3u; i++) {
    char marker = (i == ui_main_selected) ? '*' : ' ';
    UI_DrawFieldLine(8u, (uint8_t)(18u + (i * 12u)), marker, &ui_quick_fields[i]);
    UI_DrawFieldLine(68u, (uint8_t)(18u + (i * 12u)), ' ', &ui_ccs_fields[i]);
  }

  if (ui_main_selected == 3u) {
    SSD1306_DrawText(8u, 56u, "^VSTOP #NEXT *LIST");
    SSD1306_DrawText(42u, 48u, "*STOP");
  } else {
    SSD1306_DrawText(8u, 56u, "^VSEL  #NEXT *LIST");
    SSD1306_DrawText(42u, 48u, " STOP");
  }
}

static void UI_RenderParams(void)
{
  uint8_t count = (uint8_t)(sizeof(ui_all_fields) / sizeof(ui_all_fields[0]));
  uint8_t top = 0u;
  char title[16];

  if (ui_param_selected > 0u) {
    top = (uint8_t)(ui_param_selected - 1u);
  }
  if ((uint8_t)(top + 4u) > count) {
    top = (count > 4u) ? (uint8_t)(count - 4u) : 0u;
  }

  SSD1306_Clear();
  SSD1306_DrawRect(0u, 0u, SSD1306_WIDTH, SSD1306_HEIGHT);
  (void)snprintf(title, sizeof(title), "PARAMS %s", ui_param_edit ? "EDIT" : "NAV");
  SSD1306_DrawText(8u, 4u, title);

  for (uint8_t row = 0u; row < 4u; row++) {
    uint8_t index = (uint8_t)(top + row);
    if (index < count) {
      char marker = (index == ui_param_selected) ? (ui_param_edit ? '#' : '*') : ' ';
      UI_DrawFieldLine(8u, (uint8_t)(16u + (row * 11u)), marker, &ui_all_fields[index]);
    }
  }

  SSD1306_DrawText(8u, 56u, "^V SEL #ED *FS");
}

#define UI_FLASH_ITEM_COUNT 3u
#define UI_FLASH_ITEM_LOG   0u
#define UI_FLASH_ITEM_MEM   1u
#define UI_FLASH_ITEM_ERASE 2u

static const char *UI_FlashItemLabel(uint8_t idx)
{
  switch (idx) {
    case UI_FLASH_ITEM_LOG:   return "LOG";
    case UI_FLASH_ITEM_MEM:   return "MEM";
    case UI_FLASH_ITEM_ERASE: return "ERASE";
    default: return "?";
  }
}

static void UI_RenderFlash(void)
{
  char line[24];
  char val[16];

  SSD1306_Clear();
  SSD1306_DrawRect(0u, 0u, SSD1306_WIDTH, SSD1306_HEIGHT);
  SSD1306_DrawText(8u, 4u, "FLASH");

  /* Item 0: Logging on/off */
  {
    const char *label = UI_FlashItemLabel(UI_FLASH_ITEM_LOG);
    bool on = FlashLog_IsEnabled();
    const char *state = on ? "ON" : "OFF";
    char marker = (ui_flash_selected == UI_FLASH_ITEM_LOG) ? '#' : ' ';
    (void)snprintf(line, sizeof(line), "%c%s  %s", marker, label, state);
    SSD1306_DrawText(8u, 18u, line);
  }

  /* Item 1: Memory used/total */
  {
    const char *label = UI_FlashItemLabel(UI_FLASH_ITEM_MEM);
    uint32_t used = FlashLog_GetUsedBytes();
    uint32_t total = FlashLog_GetTotalBytes();
    /* Show as KB */
    uint32_t used_kb = used / 1024u;
    uint32_t total_kb = total / 1024u;
    (void)snprintf(val, sizeof(val), "%lu/%luK", (unsigned long)used_kb, (unsigned long)total_kb);
    char marker = (ui_flash_selected == UI_FLASH_ITEM_MEM) ? '#' : ' ';
    (void)snprintf(line, sizeof(line), "%c%s %s", marker, label, val);
    SSD1306_DrawText(8u, 30u, line);
  }

  /* Item 2: Erase all */
  {
    const char *label = UI_FlashItemLabel(UI_FLASH_ITEM_ERASE);
    char marker = (ui_flash_selected == UI_FLASH_ITEM_ERASE) ? '#' : ' ';
    (void)snprintf(line, sizeof(line), "%c%s", marker, label);
    SSD1306_DrawText(8u, 42u, line);
  }

  SSD1306_DrawText(8u, 56u, "^V SEL #TOG *BK");
}

static void UI_ApplyDelta(GbtRuntimeField field, int32_t delta)
{
  int32_t value = GbtGetRuntimeField(field);

  if (field == GBT27930_RUNTIME_MODE) {
    value = (value == 0x02) ? 0x01 : 0x02;
    GbtSetRuntimeField(field, value);
    return;
  }

  if (field == GBT27930_RUNTIME_PERMIT_CHARGE) {
    value = (value == 0) ? 1 : 0;
    GbtSetRuntimeField(field, value);
    return;
  }

  switch (field) {
    case GBT27930_RUNTIME_DEMAND_VOLTAGE:
    case GBT27930_RUNTIME_DEMAND_CURRENT:
    case GBT27930_RUNTIME_MODE:
    case GBT27930_RUNTIME_PACK_VOLTAGE:
    case GBT27930_RUNTIME_PACK_CURRENT:
    case GBT27930_RUNTIME_MAX_CELL_MV:
    case GBT27930_RUNTIME_MAX_CELL_GROUP:
    case GBT27930_RUNTIME_SOC_PERCENT:
    case GBT27930_RUNTIME_REMAINING_MIN:
    case GBT27930_RUNTIME_MAX_CELL_INDEX:
    case GBT27930_RUNTIME_TEMP_MAX_C:
    case GBT27930_RUNTIME_TEMP_MAX_INDEX:
    case GBT27930_RUNTIME_TEMP_MIN_C:
    case GBT27930_RUNTIME_TEMP_MIN_INDEX:
    case GBT27930_RUNTIME_PERMIT_CHARGE:
      value += delta;
      break;
    default:
      break;
  }

  GbtSetRuntimeField(field, value);
}

static int8_t UI_ButtonIndexFromPin(uint16_t gpio_pin)
{
  for (uint8_t i = 0; i < UI_BTN_COUNT; i++) {
    if (ui_buttons[i].pin == gpio_pin) {
      return (int8_t)i;
    }
  }

  return -1;
}

static bool UI_PopButtonEvent(uint8_t *button_id)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();

  uint32_t events = ui_button_events;
  bool found = false;

  if (events != 0u) {
    for (uint8_t i = 0; i < UI_BTN_COUNT; i++) {
      uint32_t mask = (1u << i);
      if ((events & mask) != 0u) {
        ui_button_events &= ~mask;
        *button_id = i;
        found = true;
        break;
      }
    }
  }

  if (primask == 0u) {
    __enable_irq();
  }

  return found;
}

static void UI_HandleMainButton(uint8_t button_id)
{
  uint8_t quick_count = (uint8_t)((sizeof(ui_quick_fields) / sizeof(ui_quick_fields[0])) + 1u);
  if (button_id == UI_BTN_UP) {
    if (ui_main_selected == 3u) {
      StopChargeManual();
    } else {
      UI_ApplyDelta(ui_quick_fields[ui_main_selected].field, ui_quick_fields[ui_main_selected].step);
    }
    ui_dirty = true;
  } else if (button_id == UI_BTN_DOWN) {
    if (ui_main_selected == 3u) {
      StopChargeManual();
    } else {
      UI_ApplyDelta(ui_quick_fields[ui_main_selected].field, -ui_quick_fields[ui_main_selected].step);
    }
    ui_dirty = true;
  } else if (button_id == UI_BTN_SHARP) {
    ui_main_selected = (uint8_t)((ui_main_selected + 1u) % quick_count);
    ui_dirty = true;
  } else if (button_id == UI_BTN_STAR) {
    ui_screen = UI_SCREEN_PARAMS;
    ui_param_edit = false;
    ui_dirty = true;
  }
}

static void UI_HandleFlashButton(uint8_t button_id)
{
  if (button_id == UI_BTN_STAR) {
    ui_screen = UI_SCREEN_MAIN;
    ui_main_selected = 0u;
    ui_dirty = true;
    return;
  }

  if (button_id == UI_BTN_UP) {
    ui_flash_selected = (ui_flash_selected == 0u) ? (UI_FLASH_ITEM_COUNT - 1u) : (ui_flash_selected - 1u);
    ui_dirty = true;
    return;
  }

  if (button_id == UI_BTN_DOWN) {
    ui_flash_selected = (ui_flash_selected + 1u) % UI_FLASH_ITEM_COUNT;
    ui_dirty = true;
    return;
  }

  /* # = toggle/select */
  if (button_id == UI_BTN_SHARP) {
    switch (ui_flash_selected) {
      case UI_FLASH_ITEM_LOG:
        FlashLog_SetEnabled(!FlashLog_IsEnabled());
        break;
      case UI_FLASH_ITEM_MEM:
        /* read-only, refresh only */
        break;
      case UI_FLASH_ITEM_ERASE:
        FlashLog_Erase();
        break;
      default:
        break;
    }
    ui_dirty = true;
  }
}

static void UI_HandleParamsButton(uint8_t button_id)
{
  uint8_t count = (uint8_t)(sizeof(ui_all_fields) / sizeof(ui_all_fields[0]));

  if (button_id == UI_BTN_STAR) {
    ui_screen = UI_SCREEN_FLASH;
    ui_param_edit = false;
    ui_flash_selected = 0u;
    ui_dirty = true;
    return;
  }

  if (button_id == UI_BTN_SHARP) {
    ui_param_edit = !ui_param_edit;
    ui_dirty = true;
    return;
  }

  if (!ui_param_edit) {
    if (button_id == UI_BTN_UP) {
      ui_param_selected = (ui_param_selected == 0u) ? (uint8_t)(count - 1u) : (uint8_t)(ui_param_selected - 1u);
      ui_dirty = true;
    } else if (button_id == UI_BTN_DOWN) {
      ui_param_selected = (uint8_t)((ui_param_selected + 1u) % count);
      ui_dirty = true;
    }
  } else {
    if (button_id == UI_BTN_UP) {
      UI_ApplyDelta(ui_all_fields[ui_param_selected].field, ui_all_fields[ui_param_selected].step);
      ui_dirty = true;
    } else if (button_id == UI_BTN_DOWN) {
      UI_ApplyDelta(ui_all_fields[ui_param_selected].field, -ui_all_fields[ui_param_selected].step);
      ui_dirty = true;
    }
  }
}

void UI_Init(void)
{
  uint32_t now = HAL_GetTick();

  for (uint8_t i = 0; i < UI_BTN_COUNT; i++) {
    ui_last_irq_at[i] = now;
  }
  ui_button_events = 0u;

  oled_ready = SSD1306_Init();
  ui_last_refresh = now;
  ui_dirty = true;
}

void UI_OnButtonInterrupt(uint16_t gpio_pin)
{
  int8_t button_idx = UI_ButtonIndexFromPin(gpio_pin);
  if (button_idx < 0) {
    return;
  }

  uint8_t idx = (uint8_t)button_idx;
  GPIO_PinState sample = HAL_GPIO_ReadPin(ui_buttons[idx].port, ui_buttons[idx].pin);
  uint32_t now = HAL_GetTick();

  if ((sample == UI_BUTTON_ACTIVE_STATE) && ((uint32_t)(now - ui_last_irq_at[idx]) >= UI_DEBOUNCE_MS)) {
    ui_last_irq_at[idx] = now;
    ui_button_events |= (1u << idx);
  }
}

void UI_Loop(void)
{
  uint8_t button_id = 0u;
  uint32_t now = HAL_GetTick();

  if (UI_PopButtonEvent(&button_id)) {
    if (ui_screen == UI_SCREEN_MAIN) {
      UI_HandleMainButton(button_id);
    } else if (ui_screen == UI_SCREEN_PARAMS) {
      UI_HandleParamsButton(button_id);
    } else {
      UI_HandleFlashButton(button_id);
    }
  }

  if ((uint32_t)(now - ui_last_refresh) >= UI_REFRESH_MS) {
    ui_last_refresh = now;
    ui_dirty = true;
  }

  if (!oled_ready && oled_last_init_try >= OLED_RETRY_INIT_MS) {
	  oled_ready = SSD1306_Init();
	  oled_last_init_try = now;
  }

  if (oled_ready && ui_dirty) {
    if (ui_screen == UI_SCREEN_MAIN) {
      UI_RenderMain();
    } else if (ui_screen == UI_SCREEN_PARAMS) {
      UI_RenderParams();
    } else {
      UI_RenderFlash();
    }

    if (SSD1306_Update()) {
      ui_dirty = false;
    }
  }
}
