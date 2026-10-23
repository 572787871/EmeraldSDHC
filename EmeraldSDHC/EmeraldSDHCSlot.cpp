//
//  EmeraldSDHCSlot.cpp
//  EmeraldSDHC card slot driver nub
//
//  Copyright © 2021-2023 Goldfish64. All rights reserved.
//

#include "EmeraldSDHCSlot.hpp"

OSDefineMetaClassAndStructors(EmeraldSDHCSlot, super);

bool EmeraldSDHCSlot::attach(IOService *provider) {
  bool     result = false;
  OSNumber *cardSlotNumber;
  OSNumber *ioUnitNumber;
  char     slotLocation[10];

  EMCheckDebugArgs();
  EMDBGLOG("Initializing EmeraldSDHCSlot");

  if (!super::attach(provider)) {
    EMSYSLOG("Superclass failed to attach");
    return false;
  }

  do {
    //
    // Get host controller provider.
    //
    _hostController = OSDynamicCast(EmeraldSDHC, provider);
    if (_hostController == nullptr) {
      EMSYSLOG("Provider is not EmeraldSDHC");
      break;
    }
    _hostController->retain();

    //
    // Get card slot ID and set location.
    //
    cardSlotNumber = OSDynamicCast(OSNumber, getProperty(kSDACardSlotNumberKey));
    if (cardSlotNumber == nullptr) {
      EMSYSLOG("Failed to get card slot number property");
      break;
    }
    _cardSlotId = cardSlotNumber->unsigned8BitValue();
    snprintf(slotLocation, sizeof (slotLocation), "%x", _cardSlotId);
    setLocation(slotLocation);
    
    //
    // Set IOUnit property with card slot ID.
    //
    ioUnitNumber = OSNumber::withNumber(_cardSlotId, 8);
    if (ioUnitNumber == nullptr) {
      EMSYSLOG("Failed to allocate IOUnit property");
      break;
    }
    setProperty("IOUnit", ioUnitNumber);
    ioUnitNumber->release();

    registerService();

    result = true;
    EMDBGLOG("Initialized EmeraldSDHCSlot on slot %u", _cardSlotId);
  } while (false);

  if (!result) {
    detach(provider);
  }
  return result;
}

void EmeraldSDHCSlot::detach(IOService *provider) {
  super::detach(provider);
}

bool EmeraldSDHCSlot::waitForBits8(UInt32 offset, UInt8 mask, bool waitClear, bool writeClear) {
  UInt32 timeout = 0;
  do {
    //
    // Wait for mask to be met.
    //
    UInt8 value = readReg8(offset) & mask;
    if (waitClear && value == 0) {
      return true;
    } else if (!waitClear && value != 0) {
      //
      // Clear bit if requested.
      //
      if (writeClear) {
        writeReg8(offset, mask);
      }
      return true;
    }

    IODelay(1);
    timeout++;
  } while (timeout <= kSDAMaskTimeout);

  EMSYSLOG("Timeout while waiting for register 0x%X", offset);
  return false;
}

bool EmeraldSDHCSlot::waitForBits16(UInt32 offset, UInt16 mask, bool waitClear, bool writeClear) {
  UInt32 timeout = 0;
  do {
    //
    // Wait for mask to be met.
    //
    UInt16 value = readReg16(offset) & mask;
    if (waitClear && value == 0) {
      return true;
    } else if (!waitClear && value != 0) {
      //
      // Clear bit if requested.
      //
      if (writeClear) {
        writeReg16(offset, mask);
      }
      return true;
    }

    IODelay(1);
    timeout++;
  } while (timeout <= kSDAMaskTimeout);

  EMSYSLOG("Timeout while waiting for register 0x%X", offset);
  return false;
}

bool EmeraldSDHCSlot::waitForBits32(UInt32 offset, UInt32 mask, bool waitClear, bool writeClear) {
  UInt32 timeout = 0;
  do {
    //
    // Wait for mask to be met.
    //
    UInt32 value = readReg32(offset) & mask;
    if (waitClear && value == 0) {
      return true;
    } else if (!waitClear && value != 0) {
      //
      // Clear bit if requested.
      //
      if (writeClear) {
        writeReg32(offset, mask);
      }
      return true;
    }

    IODelay(1);
    timeout++;
  } while (timeout <= kSDAMaskTimeout);

  EMSYSLOG("Timeout while waiting for register 0x%X", offset);
  return false;
}

const char* EmeraldSDHCSlot::getControllerVersionString() {
  switch (getControllerVersion()) {
    case kSDHostControllerVersion1_00:
      return "1.00";
    case kSDHostControllerVersion2_00:
      return "2.00";
    case kSDHostControllerVersion3_00:
      return "3.00";
    case kSDHostControllerVersion4_00:
      return "4.00";
    case kSDHostControllerVersion4_10:
      return "4.10";
    case kSDHostControllerVersion4_20:
      return "4.20";
  }

  return "Unknown";
}

bool EmeraldSDHCSlot::resetController(UInt8 bits) {
  EMDBGLOG("Resetting host controller with bits 0x%X", bits);
  writeReg8(kSDHCRegSoftwareReset, bits);

  if (!waitForBits8(kSDHCRegSoftwareReset, -1, true, false)) {
    EMSYSLOG("Host controller timed out during reset");
    return false;
  }
  EMDBGLOG("Host controller is now reset");
  return true;
}

bool EmeraldSDHCSlot::setControllerClock(UInt32 speedHz) {
  //
  // Clear existing clock register.
  //
  writeReg16(kSDHCRegClockControl, 0);
  if (speedHz == 0) {
    return true;
  }

  //
  // Get base clock speed.
  //
  UInt64 hcCaps    = readReg64(kSDHCRegCapabilities);
  UInt32 baseClock = getControllerVersion() >= kSDHostControllerVersion3_00 ?
    (hcCaps & kSDHCRegCapabilitiesBaseClockMaskVer3) >> kSDHCRegCapabilitiesBaseClockShift :
    (hcCaps & kSDHCRegCapabilitiesBaseClockMaskVer1) >> kSDHCRegCapabilitiesBaseClockShift;
  baseClock *= MHz;
  EMDBGLOG("Base clock is %u MHz", baseClock / MHz);
  if (baseClock == 0) {
    EMSYSLOG("Host controller reported an invalid base clock");
    return false;
  }

  //
  // Calculate the real divisor. SDHCI 3.0 supports any even divisor, while
  // older controllers only support powers of two. A divisor of one selects
  // the base clock directly.
  //
  UInt32 clockDiv = 1;
  if (getControllerVersion() >= kSDHostControllerVersion3_00) {
    if (baseClock > speedHz) {
      for (clockDiv = 2; clockDiv < 2046 && (baseClock / clockDiv) > speedHz; clockDiv += 2);
    }
  } else {
    for (clockDiv = 1; clockDiv < 256 && (baseClock / clockDiv) > speedHz; clockDiv <<= 1);
  }
  UInt32 actualClock = baseClock / clockDiv;
  EMDBGLOG("Clock will be set to %u %s using divisor %u",
           speedHz >= MHz ? actualClock / MHz : actualClock / kHz,
           speedHz >= MHz ? "MHz" : "kHz", clockDiv);

  //
  // SDHCI stores half of the real divisor. Program it before enabling the
  // internal clock, wait for stability, and only then start the card clock.
  //
  UInt32 encodedClockDiv = clockDiv >> 1;
  UInt16 clockControl = ((encodedClockDiv << kSDHCRegClockControlFreqSelectLowShift) & kSDHCRegClockControlFreqSelectLowMask);
  if (getControllerVersion() >= kSDHostControllerVersion3_00) {
    clockControl |= ((encodedClockDiv >> kSDHCRegClockControlFreqSelectHighRhShift) & kSDHCRegClockControlFreqSelectHighMask);
  }
  clockControl |= kSDHCRegClockControlIntClockEnable;
  writeReg16(kSDHCRegClockControl, clockControl);
  if (!waitForBits16(kSDHCRegClockControl, kSDHCRegClockControlIntClockStable, false, false)) {
    EMSYSLOG("Host controller timed out during clock startup");
    return false;
  }
  EMDBGLOG("Clock is now stable");

  writeReg16(kSDHCRegClockControl, readReg16(kSDHCRegClockControl) | kSDHCRegClockControlSDClockEnable);
  EMDBGLOG("Clock control register is now 0x%X", readReg16(kSDHCRegClockControl));
  IOSleep(50);

  return true;
}

UInt32 EmeraldSDHCSlot::getControllerClock() {
  UInt64 hcCaps    = readReg64(kSDHCRegCapabilities);
  UInt32 baseClock = getControllerVersion() >= kSDHostControllerVersion3_00 ?
    (hcCaps & kSDHCRegCapabilitiesBaseClockMaskVer3) >> kSDHCRegCapabilitiesBaseClockShift :
    (hcCaps & kSDHCRegCapabilitiesBaseClockMaskVer1) >> kSDHCRegCapabilitiesBaseClockShift;
  UInt16 clockControl = readReg16(kSDHCRegClockControl);
  UInt32 encodedClockDiv = (clockControl & kSDHCRegClockControlFreqSelectLowMask) >> kSDHCRegClockControlFreqSelectLowShift;
  if (getControllerVersion() >= kSDHostControllerVersion3_00) {
    encodedClockDiv |= (clockControl & kSDHCRegClockControlFreqSelectHighMask) << kSDHCRegClockControlFreqSelectHighRhShift;
  }
  UInt32 clockDiv = encodedClockDiv == 0 ? 1 : encodedClockDiv << 1;
  return (baseClock * MHz) / clockDiv;
}

bool EmeraldSDHCSlot::supportsHS200() {
  UInt64 hcCaps = readReg64(kSDHCRegCapabilities);
  return getControllerVersion() >= kSDHostControllerVersion3_00
    && (hcCaps & kSDHCRegCapabilitiesEmbedded8BitSupported) != 0
    && (hcCaps & kSDHCRegCapabilitiesVoltage1_8Supported) != 0
    && (hcCaps & kSDHCRegCapabilitiesSDR104Supported) != 0;
}

bool EmeraldSDHCSlot::setControllerSignalVoltage1_8() {
  if ((readReg64(kSDHCRegCapabilities) & kSDHCRegCapabilitiesVoltage1_8Supported) == 0) {
    EMSYSLOG("Host controller does not advertise 1.8V support");
    return false;
  }

  UInt16 clockControl = readReg16(kSDHCRegClockControl);
  writeReg16(kSDHCRegClockControl, clockControl & ~kSDHCRegClockControlSDClockEnable);
  IOSleep(1);

  UInt16 hcControl2 = readReg16(kSDHCRegHostControl2) | kSDHCRegHostControl21_8VSignaling;
  writeReg16(kSDHCRegHostControl2, hcControl2);
  IOSleep(5);

  bool enabled = (readReg16(kSDHCRegHostControl2) & kSDHCRegHostControl21_8VSignaling) != 0;
  writeReg16(kSDHCRegClockControl, clockControl);
  if (!enabled) {
    EMSYSLOG("Host controller rejected 1.8V signaling");
    return false;
  }

  EMDBGLOG("Host controller accepted 1.8V signaling");
  return true;
}

void EmeraldSDHCSlot::setControllerPower(bool enabled) {
  //
  // Clear power register.
  //
  writeReg16(kSDHCRegPowerControl, 0);
  if (!enabled) {
    return;
  }

  //
  // Get highest supported card voltage and enable it.
  //
  UInt64 hcCaps       = readReg64(kSDHCRegCapabilities);
  UInt16 powerControl = readReg16(kSDHCRegPowerControl);
  if (hcCaps & kSDHCRegCapabilitiesVoltage3_3Supported) {
    powerControl |= kSDHCRegPowerControlVDD1_3_3;
    EMDBGLOG("Card voltage: 3.3V");
  } else if (hcCaps & kSDHCRegCapabilitiesVoltage3_0Supported) {
    powerControl |= kSDHCRegPowerControlVDD1_3_0;
    EMDBGLOG("Card voltage: 3.0V");
  } else if (hcCaps & kSDHCRegCapabilitiesVoltage1_8Supported) {
    powerControl |= kSDHCRegPowerControlVDD1_1_8;
    EMDBGLOG("Card voltage: 1.8V");
  }
  writeReg16(kSDHCRegPowerControl, powerControl);

  //
  // Turn power on to card.
  //
  writeReg16(kSDHCRegPowerControl, readReg16(kSDHCRegPowerControl) | kSDHCRegPowerControlVDD1On);
  EMDBGLOG("Card power control register is now 0x%X", readReg16(kSDHCRegPowerControl));
  IOSleep(50);
}

void EmeraldSDHCSlot::setControllerBusWidth(SDABusWidth busWidth) {
  //
  // Set controller bus width bits.
  //
  UInt16 hcControl = readReg16(kSDHCRegHostControl1) & ~kSDHCRegHostControl1DataWidthMask;
  if (busWidth == kSDABusWidth4) {
    hcControl |= kSDHCRegHostControl1DataWidth4Bit;
    EMDBGLOG("Setting controller bus width to 4-bit mode");
  } else if (busWidth == kSDABusWidth8) {
    hcControl |= kSDHCRegHostControl1DataWidth8Bit;
    EMDBGLOG("Setting controller bus width to 8-bit mode");
  } else {
    EMDBGLOG("Setting controller bus width to 1-bit mode");
  }
  writeReg16(kSDHCRegHostControl1, hcControl);
}

void EmeraldSDHCSlot::setControllerDMAMode(SDATransferType type) {
  //
  // Set DMA mode. TODO: Support v4 controllers and 64-bit operation on supported controllers.
  //
  UInt16 hcControl = readReg16(kSDHCRegHostControl1) & ~kSDHCRegHostControl1DMA_Mask;
  if (type == kSDATransferTypeADMA2) {
    hcControl |= kSDHCRegHostControl1DMA_ADMA2_32Bit;
    EMDBGLOG("Setting controller DMA mode to 32-bit ADMA2");
  } else {
    EMDBGLOG("Setting controller DMA mode to SDMA");
  }
  writeReg16(kSDHCRegHostControl1, hcControl);
}

void EmeraldSDHCSlot::setControllerInsertionEvents(bool enable) {
  UInt16 intEnable = readReg16(kSDHCRegNormalIntSignalEnable);
  if (enable) {
    intEnable |= kSDHCRegNormalIntStatusCardInsertion | kSDHCRegNormalIntStatusCardRemoval;
  } else {
    intEnable &= ~(kSDHCRegNormalIntStatusCardInsertion | kSDHCRegNormalIntStatusCardRemoval);
  }
  writeReg16(kSDHCRegNormalIntSignalEnable, intEnable);
}
