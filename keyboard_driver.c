/* Copyright (c) 2009 Nordic Semiconductor. All Rights Reserved.
 *
 * The information contained herein is property of Nordic Semiconductor ASA.
 * Terms and conditions of usage are described in detail in NORDIC
 * SEMICONDUCTOR STANDARD SOFTWARE LICENSE AGREEMENT.
 *
 * Licensees are granted free, non-transferable use of the information. NO
 * WARRANTY of ANY KIND is provided. This heading must NOT be removed from
 * the file.
 *
 */

#include <stdint.h>
#include <stdbool.h>

#include "keyboard_driver.h"
#include "nrf.h"
#include "nrf_gpio.h"
#include "keymap.h"
#include "nrf_delay.h"

#define MODIFIER_HID_START 0xE0
#define MODIFIER_HID_END 0xE7
static uint8_t m_currently_pressed_keys[CHERRY8x16_MAX_NUM_OF_PRESSED_KEYS]; //!< Array holding currently pressed keys. Filled up from index 0. Values are
static uint8_t m_transmitted_keys[CHERRY8x16_MAX_NUM_OF_PRESSED_KEYS];       //!< Array holding the keys that have already been transmitted.
static uint8_t m_num_of_currently_pressed_keys;                              //!< Number of keys in m_currently_pressed_keys
static uint8_t m_number_of_transmitted_keys;                                 //!< Number of keys in m_transmitted_keys

static uint8_t m_key_packet[KEY_PACKET_SIZE]; //!< Stores last created key packet. One byte is used for modifier keys, one for OEMs. Key values are USB HID keycodes.

static bool cherry8x16_have_keys_changed(const uint8_t *state_now,
                                         uint8_t number_of_now_pressed_keys,
                                         const uint8_t *state_before,
                                         uint8_t number_of_before_pressed_keys);
static bool keymatrix_read(uint16_t *matrix);
static uint16_t read_column(void);
static void cherry8x16_keypacket_addkey(uint8_t key);
static void cherry8x16_keypacket_create(uint8_t *key_packet, uint8_t key_packet_size);
static void matrix_to_keycode(uint16_t *matrix, uint8_t *pressed_keys, uint8_t *number_of_pressed_keys);

void sleep_mode_prepare(void)
{
    for (uint_fast8_t i = MATRIX_ROWS; i--;)
    {
        nrf_gpio_pin_clear((uint32_t)row_pin_array[i]);
    }
    nrf_gpio_pin_set((uint32_t)row_pin_array[wakeup_button_column_index]);
    nrf_gpio_cfg_sense_input((uint32_t)column_pin_array[wakeup_button_row_index], NRF_GPIO_PIN_PULLDOWN, NRF_GPIO_PIN_SENSE_HIGH);
}

bool cherry8x16_init(void)
{
    for (uint_fast8_t i = MATRIX_ROWS; i--;)
    {
        nrf_gpio_cfg_output((uint32_t)row_pin_array[i]);
        NRF_GPIO->PIN_CNF[(uint32_t)row_pin_array[i]] |= 0x400; //Set pin to be "Disconnected 0 and standard 1"
        nrf_gpio_pin_clear((uint32_t)row_pin_array[i]);         //Set pin to low
    }
    for (uint_fast8_t i = MATRIX_COLS; i--;)
    {
        nrf_gpio_cfg_input((uint32_t)column_pin_array[i], NRF_GPIO_PIN_PULLDOWN);
    }
    return true;
}

bool new_packet(const uint8_t **p_key_packet, uint8_t *p_key_packet_size)
{
    bool new_packet_prepared;
    uint16_t key_matrix[MATRIX_ROWS];

    // Save currently pressed keys
    for (uint_fast8_t i = CHERRY8x16_MAX_NUM_OF_PRESSED_KEYS; i--;)
    {
        m_transmitted_keys[i] = m_currently_pressed_keys[i];
    }
    m_number_of_transmitted_keys = m_num_of_currently_pressed_keys;

    // Create a new packet if key states have changed and there are no keys blocking each other (ghosting/phantom keys)
    if (keymatrix_read(key_matrix))
    {
        matrix_to_keycode(key_matrix, m_currently_pressed_keys, &m_num_of_currently_pressed_keys);
        if (cherry8x16_have_keys_changed(m_currently_pressed_keys, m_num_of_currently_pressed_keys,
                                         m_transmitted_keys, m_number_of_transmitted_keys))
        {
            cherry8x16_keypacket_create(&m_key_packet[0], KEY_PACKET_SIZE);
            *p_key_packet = &m_key_packet[0];
            *p_key_packet_size = KEY_PACKET_SIZE;
            new_packet_prepared = true;
        }
        else
        {
            // The same keys are still pressed, no need to create a new packet
            new_packet_prepared = false;
        }
    }
    else
    {
        // Ghosting detected. Don't create a packet.
        new_packet_prepared = false;
    }

    return new_packet_prepared;
}

static void matrix_to_keycode(uint16_t *matrix, uint8_t *pressed_keys, uint8_t *number_of_pressed_keys)
{
    uint8_t press_normal[CHERRY8x16_MAX_NUM_OF_PRESSED_KEYS], press_fn[CHERRY8x16_MAX_NUM_OF_PRESSED_KEYS];
    uint_fast8_t count = 0;
    bool fn = false;

    *number_of_pressed_keys = 0;

    for (uint_fast8_t i = 0; i < MATRIX_ROWS; i++) // 查表
    {
        for (uint_fast8_t j = 0; j < MATRIX_COLS; j++)
        {
            if (matrix[i] & 1 << j)
            {
                press_normal[count] = keymaps[0][j][i];
                press_fn[count++] = keymaps[1][j][i];
            }
        }
    }
    for (uint_fast8_t i = 0; i < count; i++) // 查找Fn是否按下
    {
        if (press_normal[i] == KC_FN0)
        {
            fn = true;
            break;
        }
    }
    for (uint_fast8_t i = 0; i < count; i++) // 替换按键
    {
        uint_fast8_t keycode;
        if (fn && (press_fn[i] != KC_TRANSPARENT))
        {
            keycode = press_fn[i];
        }
        else
        {
            keycode = press_normal[i];
        }
        if (keycode != KC_FN0) //不要发送fn
        {
            *pressed_keys = keycode;
            pressed_keys++;
            (*number_of_pressed_keys)++;
        }
    }
}

static bool keymatrix_read(uint16_t *matrix)
{
    uint16_t matrix_debouncing[MATRIX_ROWS];
    int debouncing = 1;
    bool ghost = false;
    while (debouncing--)
    {
        for (uint_fast8_t r = 0; r < MATRIX_ROWS; r++)
        {
            nrf_gpio_pin_set((uint32_t)row_pin_array[r]);
            uint16_t cols = read_column();

            if (matrix_debouncing[r] != cols)
            {
                matrix_debouncing[r] = cols;
                debouncing = 3;
            }
            nrf_gpio_pin_clear((uint32_t)row_pin_array[r]);
        }
        nrf_delay_ms(1);
    }

    for (uint_fast8_t i = 0; i < MATRIX_ROWS; i++)
    {
        if (((matrix_debouncing[i] - 1) & matrix_debouncing[i]) > 0) //more than 1 key
        {
            for (uint_fast8_t j = 0; j < MATRIX_ROWS; j++)
            {
                if (i != j)
                    if (matrix_debouncing[i] & matrix_debouncing[j]) //check ghost key
                    {
                        ghost = true;
                        continue;
                    }
            }
        }
        matrix[i] = matrix_debouncing[i];
    }

    return !ghost;
}

static uint16_t read_column(void)
{
    uint16_t result = 0;

    for (uint_fast8_t c = 0; c < MATRIX_COLS; c++)
    {
        if (nrf_gpio_pin_read((uint32_t)column_pin_array[c]))
            result |= 1 << c;
    }

    return result;
}

/**
 * @brief Function for determining whether the keyboard matrix state has changed compared to the state before.
 *
 * @param state_now List of pressed keys in current state
 * @param number_of_now_pressed_keys Number of pressed keys in current state
 * @param state_before List of pressed keys in previous state
 * @param number_of_before_pressed_keys Number of pressed keys in previous state
 * @return
 * @retval true If keyboard matrix is different compared to state before.
 * @retval false If keyboard matrix is the same compared to state before.
 */
static bool cherry8x16_have_keys_changed(const uint8_t *state_now,
                                         uint8_t number_of_now_pressed_keys,
                                         const uint8_t *state_before,
                                         uint8_t number_of_before_pressed_keys)
{
    if (number_of_now_pressed_keys != number_of_before_pressed_keys)
    {
        return true;
    }
    else
    {
        for (uint_fast8_t i = number_of_now_pressed_keys; i--;)
        {
            if (state_now[i] != state_before[i])
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Function for adding a key to the key packet.
 *
 * If key is found to be in the packet, it will not be added twice.
 * Attempts to add more keys than the buffer capacity allows will be silently ignored.
 *
 * @param key Key to add
 */
static void cherry8x16_keypacket_addkey(uint8_t key)
{
    for (uint_fast8_t i = KEY_PACKET_KEY_INDEX; i < KEY_PACKET_SIZE; i++)
    {
        if (m_key_packet[i] == key)
        {
            return;
        }
    }

    for (uint_fast8_t i = KEY_PACKET_KEY_INDEX; i < KEY_PACKET_SIZE; i++)
    {
        if (m_key_packet[i] == KEY_PACKET_NO_KEY)
        {
            m_key_packet[i] = key;
            return;
        }
    }
}

/**
 * @brief Function for creating a new key packet.
 *
 * This function uses @ref m_currently_pressed_keys to determine pressed keys.
 * Priority is given to those keys that were found in the previous packet.
 * All modifier keys can be found in all packets.
 * If Fn key is detected to be pressed, some keys are remapped to different functions.
 *
 * @param key_packet Pointer to location where packet contents will be put
 * @param key_packet_size Key packet size in bytes
 */
static void cherry8x16_keypacket_create(uint8_t *key_packet, uint8_t key_packet_size)
{
    // Clear key_packet contents
    for (uint_fast8_t i = KEY_PACKET_KEY_INDEX; i < key_packet_size; i++)
    {
        key_packet[i] = KEY_PACKET_NO_KEY;
    }
    key_packet[KEY_PACKET_MODIFIER_KEY_INDEX] = 0;
    key_packet[KEY_PACKET_RESERVED_INDEX] = 0;

    // Give priority to keys that were already pressed when we transmitted them the last time.
    for (uint_fast8_t i = 0; i < m_number_of_transmitted_keys; i++)
    {
        for (uint_fast8_t j = 0; j < m_num_of_currently_pressed_keys; j++)
        {
            if (m_transmitted_keys[i] == m_currently_pressed_keys[j])
            {
                cherry8x16_keypacket_addkey(m_currently_pressed_keys[j]);
                break;
            }
        }
    }

    // Detect if Fn is pressed, detect modifier keys, and add rest of the keys to the packet
    for (uint_fast8_t i = 0; i < m_num_of_currently_pressed_keys; i++)
    {
        // Modifier HID usage codes are from 0xE0 to 0xE7
        if (m_currently_pressed_keys[i] >= MODIFIER_HID_START && m_currently_pressed_keys[i] <= MODIFIER_HID_END) // Detect and set modifier keys
        {
            key_packet[KEY_PACKET_MODIFIER_KEY_INDEX] |= (uint8_t)(1U << (m_currently_pressed_keys[i] - MODIFIER_HID_START));
        }
        else if (m_currently_pressed_keys[i] != 0)
        {
            cherry8x16_keypacket_addkey(m_currently_pressed_keys[i]);
        }
    }
}
