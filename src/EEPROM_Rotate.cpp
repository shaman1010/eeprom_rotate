/*

EEPROM Rotate 0.1.0

EEPRIM wrapper for ESP8266

Copyright (C) 2018 by Xose Pérez <xose dot perez at gmail dot com>

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

#include "Arduino.h"
#include "EEPROM_Rotate.h"
extern "C" uint32_t _SPIFFS_end;

// -----------------------------------------------------------------------------
// CONSTRUCTOR
// -----------------------------------------------------------------------------

EEPROM_Rotate::EEPROM_Rotate(uint32_t sector) {
    _sector = sector;
    if (0 == _sector || _sector > _getLastSector()) {
        _sector = _getLastSector();
    }
}

// -----------------------------------------------------------------------------
// PUBLIC *NEW* METHODS
// -----------------------------------------------------------------------------

bool EEPROM_Rotate::rotate(uint8_t sectors) {
    if (sectors > _getLastSector()) return false;
    _sectors = sectors;
    return true;
}

bool EEPROM_Rotate::offset(uint8_t offset) {
    if (offset + 3 > SPI_FLASH_SEC_SIZE) return false;
    _offset = offset;
    return true;
}

uint8_t EEPROM_Rotate::last() {
    return _getLastSector();
}

bool EEPROM_Rotate::erase(uint32_t sector) {
    noInterrupts();
    bool ret = (spi_flash_erase_sector(sector) == SPI_FLASH_RESULT_OK);
    interrupts();
    return ret;
}

bool EEPROM_Rotate::eraseAll() {
    bool ret = true;
    for (uint32_t index = 0; index < _sectors; index++) {
        ret = ret & erase(_getSector(index));
    }
    return ret;
}

void EEPROM_Rotate::dump(Stream & debug) {

    char ascii[17];
    memset(ascii, ' ', 16);
    ascii[16] = 0;

    debug.printf("\n         ");
    for (uint16_t i = 0; i <= 0x0F; i++) {
        debug.printf("%02X ", i);
    }
    debug.printf("\n------------------------------------------------------");

    for (uint16_t address = 0; address < SPI_FLASH_SEC_SIZE; address++) {
        if ((address % 16) == 0) {
            if (address > 0) {
                debug.print(ascii);
                memset(ascii, ' ', 16);
            }
            debug.printf("\n0x%04X:  ", address);
        }
        uint8_t data = read(address);
        if (31 < data && data < 127) ascii[address % 16] = (char) data;
        debug.printf("%02X ", data);
    }
    debug.print(ascii);

    debug.printf("\n\n");

}

// -----------------------------------------------------------------------------
// OVERWRITTEN METHODS
// -----------------------------------------------------------------------------

void EEPROM_Rotate::begin(size_t size) {

    uint32_t best_index = 0;
    uint8_t best_value = 0xFF;
    bool first = true;

    for (uint32_t index = 0; index < _sectors; index++) {

        // load the sector data
        // Sector count goes downward,
        // so default EEPROM sector is always index 0
        _sector = _getSector(index);
        EEPROMClass::begin(size);

        // get sector value
        uint8_t value = read(_offset + EEPROM_ROTATE_COUNTER_OFFSET);
        DEBUG_EEPROM_ROTATE("Magic value for sector #%u is %u\n", _sector, value);

        // validate content
        if (!_checkCRC()) {
            DEBUG_EEPROM_ROTATE("Sector #%u has not passed the CRC check\n", _sector);
            continue;
        }

        // if this is the first valid sector we are reading
        if (first) {

            first = false;
            best_index = index;
            best_value = value;

        // else compare values
        } else {

            // This new sector is newer if...
            bool newer = false;
            bool split = _sectors - 1;
            if ((value < split) && (split < best_value)) {
                newer = true;
            } else if ((best_value < split) && (split < value)) {
                newer = false;
            } else {
                newer = value > best_value;
            }

            if (newer) {
                best_index = index;
                best_value = value;
            }

        }

    }

    // If no sector has passed the checks,
    // then it might be a new installation.
    // It will then keep data from first sector (standard EEPROM).

    // Re-read the data from the best index sector
    _sector = _getSector(best_index);
    DEBUG_EEPROM_ROTATE("Sector #%u has been flagged as current\n", _sector);
    EEPROMClass::begin(size);
    _sector_value = read(_offset + EEPROM_ROTATE_COUNTER_OFFSET) + 1;

    // Update sector for next write
    _sector_index = (best_index + 1) % _sectors;
    _sector = _getSector(_sector_index);

    DEBUG_EEPROM_ROTATE("Next write sector will be #%u\n", _sector);
    DEBUG_EEPROM_ROTATE("Next magic value will be %u\n", _sector_value);

}

bool EEPROM_Rotate::commit() {

    // Check if we are really going to write
    if (!_size) return false;
    if (!_dirty) return true;
    if (!_data) return false;

    // Update the counter & crc bytes
    uint16_t crc = _calculateCRC();
    write(_offset + EEPROM_ROTATE_CRC_OFFSET, (crc >> 8) & 0xFF);
    write(_offset + EEPROM_ROTATE_CRC_OFFSET + 1, crc & 0xFF);
    write(_offset + EEPROM_ROTATE_COUNTER_OFFSET, _sector_value);

    // Perform the commit
    bool ret = EEPROMClass::commit();

    // If commit succeeded...
    if (ret) {

        // Update next value
        _sector_value++;

        // Update sector for next write
        _sector_index = (_sector_index + 1) % _sectors;
        _sector = _getSector(_sector_index);

        DEBUG_EEPROM_ROTATE("Next write sector will be #%u\n", _sector);
        DEBUG_EEPROM_ROTATE("Next magic value will be %u\n", _sector_value);

    }

    return ret;

}

// -----------------------------------------------------------------------------
// PRIVATE METHODS
// -----------------------------------------------------------------------------

uint32_t EEPROM_Rotate::_getLastSector() {
    return ((uint32_t)&_SPIFFS_end - 0x40200000) / SPI_FLASH_SEC_SIZE;
}

uint32_t EEPROM_Rotate::_getSector(uint8_t index) {
    return _getLastSector() - index;
}

uint16_t EEPROM_Rotate::_calculateCRC() {
    uint16_t crc = 0;
    for (uint16_t address = 0; address < _size; address++) {
        if (_offset <= address && address <= _offset + 2) continue;
        crc = crc + read(address);
    }
    return crc;
}

bool EEPROM_Rotate::_checkCRC() {
    uint16_t calculated = _calculateCRC();
    uint16_t stored =
        (read(_offset + EEPROM_ROTATE_CRC_OFFSET) << 8) +
        read(_offset + EEPROM_ROTATE_CRC_OFFSET + 1);
    DEBUG_EEPROM_ROTATE("Calculated CRC: 0x%04X\n", calculated);
    DEBUG_EEPROM_ROTATE("Stored CRC    : 0x%04X\n", stored);
    return (calculated == stored);
}