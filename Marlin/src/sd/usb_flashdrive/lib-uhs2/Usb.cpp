/**
 * Copyright (C) 2011 Circuits At Home, LTD. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Contact information
 * -------------------
 *
 * Circuits At Home, LTD
 * Web      :  https://www.circuitsathome.com
 * e-mail   :  support@circuitsathome.com
 */

//
// USB functions supporting Flash Drive
//

#include "../../../inc/MarlinConfigPre.h"

#if ALL(HAS_USB_FLASH_DRIVE, USE_UHS2_USB)

#include "Usb.h"

static uint8_t usb_error = 0;
static uint8_t usb_task_state;

/* constructor */
USB::USB() : bmHubPre(0) {
  usb_task_state = USB_DETACHED_SUBSTATE_INITIALIZE; // Set up state machine
  init();
}

/* Initialize data structures */
void USB::init() {
  //devConfigIndex = 0;
  bmHubPre = 0;
}

uint8_t USB::getUsbTaskState() { return usb_task_state; }
void USB::setUsbTaskState(uint8_t state) { usb_task_state = state; }

EpInfo* USB::getEpInfoEntry(uint8_t addr, uint8_t ep) {
  UsbDevice *p = addrPool.GetUsbDevicePtr(addr);

  if (!p || !p->epinfo)
    return nullptr;

  EpInfo *pep = p->epinfo;

  for (uint8_t i = 0; i < p->epcount; i++) {
    if ((pep)->epAddr == ep)
      return pep;

    pep++;
  }
  return nullptr;
}

/**
 * Set device table entry
 * Each device is different and has different number of endpoints.
 * This function plugs endpoint record structure, defined in application, to devtable
 */
uint8_t USB::setEpInfoEntry(uint8_t addr, uint8_t epcount, EpInfo* eprecord_ptr) {
  if (!eprecord_ptr)
    return USB_ERROR_INVALID_ARGUMENT;

  UsbDevice *p = addrPool.GetUsbDevicePtr(addr);

  if (!p)
    return USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL;

  p->address.devAddress = addr;
  p->epinfo = eprecord_ptr;
  p->epcount = epcount;

  return 0;
}

uint8_t USB::SetAddress(uint8_t addr, uint8_t ep, EpInfo **ppep, uint16_t *nak_limit) {
  UsbDevice *p = addrPool.GetUsbDevicePtr(addr);

  if (!p)
    return USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL;

  if (!p->epinfo)
    return USB_ERROR_EPINFO_IS_NULL;

  *ppep = getEpInfoEntry(addr, ep);

  if (!*ppep)
    return USB_ERROR_EP_NOT_FOUND_IN_TBL;

  *nak_limit = (0x0001UL << (((*ppep)->bmNakPower > USB_NAK_MAX_POWER) ? USB_NAK_MAX_POWER : (*ppep)->bmNakPower));
  (*nak_limit)--;
  /*
    USBTRACE2("\r\nAddress: ", addr);
    USBTRACE2(" EP: ", ep);
    USBTRACE2(" NAK Power: ",(*ppep)->bmNakPower);
    USBTRACE2(" NAK Limit: ", nak_limit);
    USBTRACE("\r\n");
   */
  regWr(rPERADDR, addr); // Set peripheral address

  uint8_t mode = regRd(rMODE);

  //Serial.print("\r\nMode: ");
  //Serial.println( mode, HEX);
  //Serial.print("\r\nLS: ");
  //Serial.println(p->lowspeed, HEX);

  // Set bmLOWSPEED and bmHUBPRE in case of low-speed device, reset them otherwise
  regWr(rMODE, (p->lowspeed) ? mode | bmLOWSPEED | bmHubPre : mode & ~(bmHUBPRE | bmLOWSPEED));

  return 0;
}

/* Control transfer. Sets address, endpoint, fills control packet with necessary data, dispatches control packet, and initiates bulk IN transfer,   */
/* depending on request. Actual requests are defined as inlines                                                                                      */
/* return codes:                */
/* 00       =   success         */
/* 01-0f    =   non-zero HRSLT  */
uint8_t USB::ctrlReq(uint8_t addr, uint8_t ep, uint8_t bmReqType, uint8_t bRequest, uint8_t wValLo, uint8_t wValHi,
  uint16_t wInd, uint16_t total, uint16_t nbytes, uint8_t *dataptr, USBReadParser *p) {
  bool direction = false; // Request direction, IN or OUT
  uint8_t rcode;
  SETUP_PKT setup_pkt;

  EpInfo *pep = nullptr;
  uint16_t nak_limit = 0;

  rcode = SetAddress(addr, ep, &pep, &nak_limit);
  if (rcode) return rcode;

  direction = ((bmReqType & 0x80) > 0);

  /* fill in setup packet */
  setup_pkt.ReqType_u.bmRequestType = bmReqType;
  setup_pkt.bRequest = bRequest;
  setup_pkt.wVal_u.wValueLo = wValLo;
  setup_pkt.wVal_u.wValueHi = wValHi;
  setup_pkt.wIndex = wInd;
  setup_pkt.wLength = total;

  bytesWr(rSUDFIFO, 8, (uint8_t*) & setup_pkt); // Transfer to setup packet FIFO

  rcode = dispatchPkt(tokSETUP, ep, nak_limit); // Dispatch packet
  if (rcode) return rcode; // Return HRSLT if not zero

  if (dataptr) { // Data stage, if present
    if (direction) { // IN transfer
      uint16_t left = total;
      pep->bmRcvToggle = 1; // BmRCVTOG1;

      while (left) {
        // Bytes read into buffer
        uint16_t read = nbytes;
        //uint16_t read = (left<nbytes) ? left : nbytes;

        rcode = InTransfer(pep, nak_limit, &read, dataptr);
        if (rcode == hrTOGERR) {
          // Yes, we flip it wrong here so that next time it is actually correct!
          pep->bmRcvToggle = (regRd(rHRSL) & bmSNDTOGRD) ? 0 : 1;
          continue;
        }

        if (rcode) return rcode;

        // Invoke callback function if inTransfer completed successfully and callback function pointer is specified
        if (!rcode && p) ((USBReadParser*)p)->Parse(read, dataptr, total - left);

        left -= read;

        if (read < nbytes) break;
      }
    }
    else { // OUT transfer
      pep->bmSndToggle = 1; // BmSNDTOG1;
      rcode = OutTransfer(pep, nak_limit, nbytes, dataptr);
    }
    if (rcode) return rcode; // Return error
  }
  // Status stage
  return dispatchPkt((direction) ? tokOUTHS : tokINHS, ep, nak_limit); // GET if direction
}

/**
 * IN transfer to arbitrary endpoint. Assumes PERADDR is set. Handles multiple packets if necessary. Transfers 'nbytes' bytes.
 * Keep sending INs and writes data to memory area pointed by 'data'
 * rcode 0 if no errors. rcode 01-0f is relayed from dispatchPkt(). Rcode f0 means RCVDAVIRQ error, fe = USB xfer timeout
 */
uint8_t USB::inTransfer(uint8_t addr, uint8_t ep, uint16_t *nbytesptr, uint8_t *data, uint8_t bInterval/*=0*/) {
  EpInfo *pep = nullptr;
  uint16_t nak_limit = 0;

  uint8_t rcode = SetAddress(addr, ep, &pep, &nak_limit);
  if (rcode) {
    USBTRACE3("(USB::InTransfer) SetAddress Failed ", rcode, 0x81);
    USBTRACE3("(USB::InTransfer) addr requested ", addr, 0x81);
    USBTRACE3("(USB::InTransfer) ep requested ", ep, 0x81);
    return rcode;
  }
  return InTransfer(pep, nak_limit, nbytesptr, data, bInterval);
}

uint8_t USB::InTransfer(EpInfo *pep, uint16_t nak_limit, uint16_t *nbytesptr, uint8_t *data, uint8_t bInterval/*=0*/) {
  uint8_t rcode = 0;
  uint8_t pktsize;

  uint16_t nbytes = *nbytesptr;
  //printf("Requesting %i bytes ", nbytes);
  uint8_t maxpktsize = pep->maxPktSize;

  *nbytesptr = 0;
  regWr(rHCTL, (pep->bmRcvToggle) ? bmRCVTOG1 : bmRCVTOG0); // Set toggle value

  // Use a 'break' to exit this loop
  for (;;) {
    rcode = dispatchPkt(tokIN, pep->epAddr, nak_limit); // IN packet to EP-'endpoint'. Function takes care of NAKS.
    if (rcode == hrTOGERR) {
      // Yes, we flip it wrong here so that next time it is actually correct!
      pep->bmRcvToggle = (regRd(rHRSL) & bmRCVTOGRD) ? 0 : 1;
      regWr(rHCTL, (pep->bmRcvToggle) ? bmRCVTOG1 : bmRCVTOG0); // Set toggle value
      continue;
    }
    if (rcode) {
      //printf(">>>>>>>> Problem! dispatchPkt %2.2x\r\n", rcode);
      break; // Should be 0, indicating ACK. Else return error code.
    }
    /* check for RCVDAVIRQ and generate error if not present */
    /* the only case when absence of RCVDAVIRQ makes sense is when toggle error occurred. Need to add handling for that */
    if ((regRd(rHIRQ) & bmRCVDAVIRQ) == 0) {
      //printf(">>>>>>>> Problem! NO RCVDAVIRQ!\r\n");
      rcode = 0xF0; // Receive error
      break;
    }
    pktsize = regRd(rRCVBC); // Number of received bytes
    //printf("Got %i bytes \r\n", pktsize);
    // This would be OK, but...
    //assert(pktsize <= nbytes);
    if (pktsize > nbytes) {
      // This can happen. Use of assert on Arduino locks up the Arduino.
      // So I will trim the value, and hope for the best.
      //printf(">>>>>>>> Problem! Wanted %i bytes but got %i.\r\n", nbytes, pktsize);
      pktsize = nbytes;
    }

    int16_t mem_left = (int16_t)nbytes - *((int16_t*)nbytesptr);
    if (mem_left < 0) mem_left = 0;

    data = bytesRd(rRCVFIFO, ((pktsize > mem_left) ? mem_left : pktsize), data);

    regWr(rHIRQ, bmRCVDAVIRQ); // Clear the IRQ & free the buffer
    *nbytesptr += pktsize; // Add this packet's byte count to total transfer length

    /* The transfer is complete under two conditions:           */
    /* 1. The device sent a short packet (L.T. maxPacketSize)   */
    /* 2. 'nbytes' have been transferred.                       */
    if (pktsize < maxpktsize || *nbytesptr >= nbytes) { // Transferred 'nbytes' bytes?
      // Save toggle value
      pep->bmRcvToggle = ((regRd(rHRSL) & bmRCVTOGRD)) ? 1 : 0;
      //printf("\r\n");
      rcode = 0;
      break;
    }
    else if (bInterval > 0)
      delay(bInterval); // Delay according to polling interval
  }
  return rcode;
}

/**
 * OUT transfer to arbitrary endpoint. Handles multiple packets if necessary. Transfers 'nbytes' bytes.
 * Handles NAK bug per Maxim Application Note 4000 for single buffer transfer
 * rcode 0 if no errors. rcode 01-0f is relayed from HRSL
 */
uint8_t USB::outTransfer(uint8_t addr, uint8_t ep, uint16_t nbytes, uint8_t *data) {
  EpInfo *pep = nullptr;
  uint16_t nak_limit = 0;

  uint8_t rcode = SetAddress(addr, ep, &pep, &nak_limit);
  if (rcode) return rcode;

  return OutTransfer(pep, nak_limit, nbytes, data);
}

uint8_t USB::OutTransfer(EpInfo *pep, uint16_t nak_limit, uint16_t nbytes, uint8_t *data) {
  uint8_t rcode = hrSUCCESS, retry_count;
  uint8_t *data_p = data; // Local copy of the data pointer
  uint16_t bytes_tosend, nak_count;
  uint16_t bytes_left = nbytes;

  uint8_t maxpktsize = pep->maxPktSize;

  if (maxpktsize < 1 || maxpktsize > 64)
    return USB_ERROR_INVALID_MAX_PKT_SIZE;

  uint32_t timeout = (uint32_t)millis() + USB_XFER_TIMEOUT;

  regWr(rHCTL, (pep->bmSndToggle) ? bmSNDTOG1 : bmSNDTOG0); // Set toggle value

  while (bytes_left) {
    retry_count = 0;
    nak_count = 0;
    bytes_tosend = (bytes_left >= maxpktsize) ? maxpktsize : bytes_left;
    bytesWr(rSNDFIFO, bytes_tosend, data_p); // Filling output FIFO
    regWr(rSNDBC, bytes_tosend); // Set number of bytes
    regWr(rHXFR, (tokOUT | pep->epAddr)); // Dispatch packet
    while (!(regRd(rHIRQ) & bmHXFRDNIRQ)); // Wait for the completion IRQ
    regWr(rHIRQ, bmHXFRDNIRQ); // Clear IRQ
    rcode = (regRd(rHRSL) & 0x0F);

    while (rcode && ((int32_t)((uint32_t)millis() - timeout) < 0L)) {
      switch (rcode) {
        case hrNAK:
          nak_count++;
          if (nak_limit && (nak_count == nak_limit))
            goto breakout;
          //return rcode;
          break;
        case hrTIMEOUT:
          retry_count++;
          if (retry_count == USB_RETRY_LIMIT)
            goto breakout;
          //return rcode;
          break;
        case hrTOGERR:
          // Yes, we flip it wrong here so that next time it is actually correct!
          pep->bmSndToggle = (regRd(rHRSL) & bmSNDTOGRD) ? 0 : 1;
          regWr(rHCTL, (pep->bmSndToggle) ? bmSNDTOG1 : bmSNDTOG0); // Set toggle value
          break;
        default:
          goto breakout;
      }

      /* process NAK according to Host out NAK bug */
      regWr(rSNDBC, 0);
      regWr(rSNDFIFO, *data_p);
      regWr(rSNDBC, bytes_tosend);
      regWr(rHXFR, (tokOUT | pep->epAddr)); // Dispatch packet
      while (!(regRd(rHIRQ) & bmHXFRDNIRQ)); // Wait for the completion IRQ
      regWr(rHIRQ, bmHXFRDNIRQ); // Clear IRQ
      rcode = (regRd(rHRSL) & 0x0F);
    } // While rcode && ....
    bytes_left -= bytes_tosend;
    data_p += bytes_tosend;
  } // While bytes_left...
breakout:

  pep->bmSndToggle = (regRd(rHRSL) & bmSNDTOGRD) ? 1 : 0; // BmSNDTOG1 : bmSNDTOG0;  // Update toggle
  return ( rcode); // Should be 0 in all cases
}

/**
 * Dispatch USB packet. Assumes peripheral address is set and relevant buffer is loaded/empty
 * If NAK, tries to re-send up to nak_limit times
 * If nak_limit == 0, do not count NAKs, exit after timeout
 * If bus timeout, re-sends up to USB_RETRY_LIMIT times
 * return codes 0x00-0x0F are HRSLT( 0x00 being success ), 0xFF means timeout
 */
uint8_t USB::dispatchPkt(uint8_t token, uint8_t ep, uint16_t nak_limit) {
  uint32_t timeout = (uint32_t)millis() + USB_XFER_TIMEOUT;
  uint8_t tmpdata;
  uint8_t rcode = hrSUCCESS;
  uint8_t retry_count = 0;
  uint16_t nak_count = 0;

  while ((int32_t)((uint32_t)millis() - timeout) < 0L) {
    #if defined(ESP8266) || defined(ESP32)
      yield(); // Needed in order to reset the watchdog timer on the ESP8266
    #endif
    regWr(rHXFR, (token | ep)); // Launch the transfer
    rcode = USB_ERROR_TRANSFER_TIMEOUT;

    while ((int32_t)((uint32_t)millis() - timeout) < 0L) { // Wait for transfer completion
      #if defined(ESP8266) || defined(ESP32)
        yield(); // Needed to reset the watchdog timer on the ESP8266
      #endif
      tmpdata = regRd(rHIRQ);

      if (tmpdata & bmHXFRDNIRQ) {
        regWr(rHIRQ, bmHXFRDNIRQ); // Clear the interrupt
        rcode = 0x00;
        break;
      }

    } // While millis() < timeout

    //if (rcode != 0x00) return rcode; // Exit if timeout

    rcode = (regRd(rHRSL) & 0x0F); // Analyze transfer result

    switch (rcode) {
      case hrNAK:
        nak_count++;
        if (nak_limit && (nak_count == nak_limit))
          return (rcode);
        break;
      case hrTIMEOUT:
        retry_count++;
        if (retry_count == USB_RETRY_LIMIT)
          return (rcode);
        break;
      default:
        return (rcode);
    }

  } // While timeout > millis()
  return rcode;
}

// USB main task. Performs enumeration/cleanup
void USB::Task() { // USB state machine
  uint8_t rcode;
  uint8_t tmpdata;
  static uint32_t delay = 0;
  //USB_FD_DEVICE_DESCRIPTOR buf;
  bool lowspeed = false;

  MAX3421E::Task();

  tmpdata = getVbusState();

  /* modify USB task state if Vbus changed */
  switch (tmpdata) {
    case SE1: // Illegal state
      usb_task_state = USB_DETACHED_SUBSTATE_ILLEGAL;
      lowspeed = false;
      break;
    case SE0: // Disconnected
      if ((usb_task_state & USB_STATE_MASK) != USB_STATE_DETACHED)
        usb_task_state = USB_DETACHED_SUBSTATE_INITIALIZE;
      lowspeed = false;
      break;
    case LSHOST:
      lowspeed = true;
      // Intentional fallthrough
    case FSHOST: // Attached
      if ((usb_task_state & USB_STATE_MASK) == USB_STATE_DETACHED) {
        delay = (uint32_t)millis() + USB_SETTLE_DELAY;
        usb_task_state = USB_ATTACHED_SUBSTATE_SETTLE;
      }
      break;
  }

  for (uint8_t i = 0; i < USB_NUMDEVICES; i++)
    if (devConfig[i]) rcode = devConfig[i]->Poll();

  switch (usb_task_state) {
    case USB_DETACHED_SUBSTATE_INITIALIZE:
      init();

      for (uint8_t i = 0; i < USB_NUMDEVICES; i++)
        if (devConfig[i])
          rcode = devConfig[i]->Release();

      usb_task_state = USB_DETACHED_SUBSTATE_WAIT_FOR_DEVICE;
      break;
    case USB_DETACHED_SUBSTATE_WAIT_FOR_DEVICE: // Just sit here
      break;
    case USB_DETACHED_SUBSTATE_ILLEGAL: // Just sit here
      break;
    case USB_ATTACHED_SUBSTATE_SETTLE: // Settle time for just attached device
      if ((int32_t)((uint32_t)millis() - delay) >= 0L)
        usb_task_state = USB_ATTACHED_SUBSTATE_RESET_DEVICE;
      else break; // Don't fall through
    case USB_ATTACHED_SUBSTATE_RESET_DEVICE:
      regWr(rHCTL, bmBUSRST); // Issue bus reset
      usb_task_state = USB_ATTACHED_SUBSTATE_WAIT_RESET_COMPLETE;
      break;
    case USB_ATTACHED_SUBSTATE_WAIT_RESET_COMPLETE:
      if ((regRd(rHCTL) & bmBUSRST) == 0) {
        tmpdata = regRd(rMODE) | bmSOFKAENAB; // Start SOF generation
        regWr(rMODE, tmpdata);
        usb_task_state = USB_ATTACHED_SUBSTATE_WAIT_SOF;
        //delay = (uint32_t)millis() + 20; // 20ms wait after reset per USB spec
      }
      break;
    case USB_ATTACHED_SUBSTATE_WAIT_SOF: // Todo: change check order
      if (regRd(rHIRQ) & bmFRAMEIRQ) {
        // When first SOF received _and_ 20ms has passed we can continue
        /*
          if (delay < (uint32_t)millis()) // 20ms passed
            usb_task_state = USB_STATE_CONFIGURING;
        */
        usb_task_state = USB_ATTACHED_SUBSTATE_WAIT_RESET;
        delay = (uint32_t)millis() + 20;
      }
      break;
    case USB_ATTACHED_SUBSTATE_WAIT_RESET:
      if ((int32_t)((uint32_t)millis() - delay) >= 0L) usb_task_state = USB_STATE_CONFIGURING;
      else break; // Don't fall through
    case USB_STATE_CONFIGURING:

      //Serial.print("\r\nConf.LS: ");
      //Serial.println(lowspeed, HEX);

      rcode = Configuring(0, 0, lowspeed);

      if (!rcode)
        usb_task_state = USB_STATE_RUNNING;
      else if (rcode != USB_DEV_CONFIG_ERROR_DEVICE_INIT_INCOMPLETE) {
        usb_error = rcode;
        usb_task_state = USB_STATE_ERROR;
      }
      break;
    case USB_STATE_RUNNING:
      break;
    case USB_STATE_ERROR:
      //MAX3421E::Init();
      break;
  }
}

uint8_t USB::DefaultAddressing(uint8_t parent, uint8_t port, bool lowspeed) {
  //uint8_t                buf[12];
  uint8_t rcode;
  UsbDevice *p0 = nullptr, *p = nullptr;

  // Get pointer to pseudo device with address 0 assigned
  p0 = addrPool.GetUsbDevicePtr(0);
  if (!p0) return USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL;
  if (!p0->epinfo) return USB_ERROR_EPINFO_IS_NULL;

  p0->lowspeed = lowspeed;

  // Allocate new address according to device class
  uint8_t bAddress = addrPool.AllocAddress(parent, false, port);
  if (!bAddress) return USB_ERROR_OUT_OF_ADDRESS_SPACE_IN_POOL;

  p = addrPool.GetUsbDevicePtr(bAddress);
  if (!p) return USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL;

  p->lowspeed = lowspeed;

  // Assign new address to the device
  rcode = setAddr(0, 0, bAddress);
  if (rcode) {
    addrPool.FreeAddress(bAddress);
    bAddress = 0;
  }
  return rcode;
}

uint8_t USB::AttemptConfig(uint8_t driver, uint8_t parent, uint8_t port, bool lowspeed) {
  //printf("AttemptConfig: parent = %i, port = %i\r\n", parent, port);
  uint8_t retries = 0;

again:
  uint8_t rcode = devConfig[driver]->ConfigureDevice(parent, port, lowspeed);
  if (rcode == USB_ERROR_CONFIG_REQUIRES_ADDITIONAL_RESET) {
    if (parent == 0) {
      // Send a bus reset on the root interface.
      regWr(rHCTL, bmBUSRST); // Issue bus reset
      delay(102); // Delay 102ms, compensate for clock inaccuracy.
    }
    else {
      // Reset parent port
      devConfig[parent]->ResetHubPort(port);
    }
  }
  else if (rcode == hrJERR && retries < 3) { // Some devices returns this when plugged in - trying to initialize the device again usually works
    delay(100);
    retries++;
    goto again;
  }
  else if (rcode)
    return rcode;

  rcode = devConfig[driver]->Init(parent, port, lowspeed);
  if (rcode == hrJERR && retries < 3) { // Some devices returns this when plugged in - trying to initialize the device again usually works
    delay(100);
    retries++;
    goto again;
  }

  if (rcode) {
    // Issue a bus reset, because the device may be in a limbo state
    if (parent == 0) {
      // Send a bus reset on the root interface.
      regWr(rHCTL, bmBUSRST); // Issue bus reset
      delay(102); // Delay 102ms, compensate for clock inaccuracy.
    }
    else {
      // Reset parent port
      devConfig[parent]->ResetHubPort(port);
    }
  }
  return rcode;
}

/**
 * This is broken. It needs to enumerate differently.
 * It causes major problems with several devices if detected in an unexpected order.
 *
 * Oleg - I wouldn't do anything before the newly connected device is considered sane.
 * i.e.(delays are not indicated for brevity):
 * 1. reset
 * 2. GetDevDescr();
 * 3a. If ACK, continue with allocating address, addressing, etc.
 * 3b. Else reset again, count resets, stop at some number (5?).
 * 4. When max.number of resets is reached, toggle power/fail
 * If desired, this could be modified by performing two resets with GetDevDescr() in the middle - however, from my experience, if a device answers to GDD()
 * it doesn't need to be reset again
 * New steps proposal:
 * 1: get address pool instance. exit on fail
 * 2: pUsb->getDevDescr(0, 0, constBufSize, (uint8_t*)buf). exit on fail.
 * 3: bus reset, 100ms delay
 * 4: set address
 * 5: pUsb->setEpInfoEntry(bAddress, 1, epInfo), exit on fail
 * 6: while (configurations) {
 *      for (each configuration) {
 *        for (each driver) {
 *           6a: Ask device if it likes configuration. Returns 0 on OK.
 *             If successful, the driver configured device.
 *             The driver now owns the endpoints, and takes over managing them.
 *             The following will need codes:
 *                 Everything went well, instance consumed, exit with success.
 *                 Instance already in use, ignore it, try next driver.
 *                 Not a supported device, ignore it, try next driver.
 *                 Not a supported configuration for this device, ignore it, try next driver.
 *                 Could not configure device, fatal, exit with fail.
 *        }
 *      }
 *    }
 * 7: for (each driver) {
 *      7a: Ask device if it knows this VID/PID. Acts exactly like 6a, but using VID/PID
 * 8: if we get here, no driver likes the device plugged in, so exit failure.
 */
uint8_t USB::Configuring(uint8_t parent, uint8_t port, bool lowspeed) {
  //uint8_t bAddress = 0;
  //printf("Configuring: parent = %i, port = %i\r\n", parent, port);
  uint8_t devConfigIndex;
  uint8_t rcode = 0;
  uint8_t buf[sizeof (USB_FD_DEVICE_DESCRIPTOR)];
  USB_FD_DEVICE_DESCRIPTOR *udd = reinterpret_cast<USB_FD_DEVICE_DESCRIPTOR *>(buf);
  UsbDevice *p = nullptr;
  EpInfo *oldep_ptr = nullptr;
  EpInfo epInfo;

  epInfo.epAddr = 0;
  epInfo.maxPktSize = 8;
  epInfo.bmSndToggle = 0;
  epInfo.bmRcvToggle = 0;
  epInfo.bmNakPower = USB_NAK_MAX_POWER;

  //delay(2000);
  AddressPool &addrPool = GetAddressPool();
  // Get pointer to pseudo device with address 0 assigned
  p = addrPool.GetUsbDevicePtr(0);
  if (!p) {
    //printf("Configuring error: USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL\r\n");
    return USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL;
  }

  // Save old pointer to EP_RECORD of address 0
  oldep_ptr = p->epinfo;

  // Temporary assign new pointer to epInfo to p->epinfo in order to
  // Avoid toggle inconsistence

  p->epinfo = &epInfo;

  p->lowspeed = lowspeed;
  // Get device descriptor
  rcode = getDevDescr(0, 0, sizeof (USB_FD_DEVICE_DESCRIPTOR), (uint8_t*)buf);

  // Restore p->epinfo
  p->epinfo = oldep_ptr;

  if (rcode) {
    //printf("Configuring error: Can't get USB_FD_DEVICE_DESCRIPTOR\r\n");
    return rcode;
  }

  // To-do?
  // Allocate new address according to device class
  //bAddress = addrPool.AllocAddress(parent, false, port);

  uint16_t vid = udd->idVendor, pid = udd->idProduct;
  uint8_t klass = udd->bDeviceClass, subklass = udd->bDeviceSubClass;

  // Attempt to configure if VID/PID or device class matches with a driver
  // Qualify with subclass too.
  //
  // VID/PID & class tests default to false for drivers not yet ported
  // Subclass defaults to true, so you don't have to define it if you don't have to.
  //
  for (devConfigIndex = 0; devConfigIndex < USB_NUMDEVICES; devConfigIndex++) {
    if (!devConfig[devConfigIndex]) continue; // No driver
    if (devConfig[devConfigIndex]->GetAddress()) continue; // Consumed
    if (devConfig[devConfigIndex]->DEVSUBCLASSOK(subklass) && (devConfig[devConfigIndex]->VIDPIDOK(vid, pid) || devConfig[devConfigIndex]->DEVCLASSOK(klass))) {
      rcode = AttemptConfig(devConfigIndex, parent, port, lowspeed);
      if (rcode != USB_DEV_CONFIG_ERROR_DEVICE_NOT_SUPPORTED)
        break;
    }
  }

  if (devConfigIndex < USB_NUMDEVICES) return rcode;

  // Blindly attempt to configure
  for (devConfigIndex = 0; devConfigIndex < USB_NUMDEVICES; devConfigIndex++) {
    if (!devConfig[devConfigIndex]) continue;
    if (devConfig[devConfigIndex]->GetAddress()) continue; // Consumed
    if (devConfig[devConfigIndex]->DEVSUBCLASSOK(subklass) && (devConfig[devConfigIndex]->VIDPIDOK(vid, pid) || devConfig[devConfigIndex]->DEVCLASSOK(klass))) continue; // If this is true it means it must have returned USB_DEV_CONFIG_ERROR_DEVICE_NOT_SUPPORTED above
    rcode = AttemptConfig(devConfigIndex, parent, port, lowspeed);

    //printf("ERROR ENUMERATING %2.2x\r\n", rcode);
    if (!(rcode == USB_DEV_CONFIG_ERROR_DEVICE_NOT_SUPPORTED || rcode == USB_ERROR_CLASS_INSTANCE_ALREADY_IN_USE)) {
      // In case of an error dev_index should be reset to 0
      // in order to start from the very beginning the
      // next time the program gets here
      //if (rcode != USB_DEV_CONFIG_ERROR_DEVICE_INIT_INCOMPLETE)
      //devConfigIndex = 0;
      return rcode;
    }
  }
  // Arriving here means the device class is unsupported by registered classes
  return DefaultAddressing(parent, port, lowspeed);
}

uint8_t USB::ReleaseDevice(uint8_t addr) {
  if (addr) {
    for (uint8_t i = 0; i < USB_NUMDEVICES; i++) {
      if (!devConfig[i]) continue;
      if (devConfig[i]->GetAddress() == addr)
        return devConfig[i]->Release();
    }
  }
  return 0;
}

// Get device descriptor
uint8_t USB::getDevDescr(uint8_t addr, uint8_t ep, uint16_t nbytes, uint8_t *dataptr) {
  return ctrlReq(addr, ep, bmREQ_GET_DESCR, USB_REQUEST_GET_DESCRIPTOR, 0x00, USB_DESCRIPTOR_DEVICE, 0x0000, nbytes, nbytes, dataptr, nullptr);
}

// Get configuration descriptor
uint8_t USB::getConfDescr(uint8_t addr, uint8_t ep, uint16_t nbytes, uint8_t conf, uint8_t *dataptr) {
  return ctrlReq(addr, ep, bmREQ_GET_DESCR, USB_REQUEST_GET_DESCRIPTOR, conf, USB_DESCRIPTOR_CONFIGURATION, 0x0000, nbytes, nbytes, dataptr, nullptr);
}

/**
 * Requests Configuration Descriptor. Sends two Get Conf Descr requests.
 * The first one gets the total length of all descriptors, then the second one requests this
 * total length. The length of the first request can be shorter (4 bytes), however, there are
 * devices which won't work unless this length is set to 9.
 */
uint8_t USB::getConfDescr(uint8_t addr, uint8_t ep, uint8_t conf, USBReadParser *p) {
  const uint8_t bufSize = 64;
  uint8_t buf[bufSize];
  USB_FD_CONFIGURATION_DESCRIPTOR *ucd = reinterpret_cast<USB_FD_CONFIGURATION_DESCRIPTOR *>(buf);

  uint8_t ret = getConfDescr(addr, ep, 9, conf, buf);
  if (ret) return ret;

  uint16_t total = ucd->wTotalLength;

  //USBTRACE2("\r\ntotal conf.size:", total);

  return ctrlReq(addr, ep, bmREQ_GET_DESCR, USB_REQUEST_GET_DESCRIPTOR, conf, USB_DESCRIPTOR_CONFIGURATION, 0x0000, total, bufSize, buf, p);
}

// Get string descriptor
uint8_t USB::getStrDescr(uint8_t addr, uint8_t ep, uint16_t ns, uint8_t index, uint16_t langid, uint8_t *dataptr) {
  return ctrlReq(addr, ep, bmREQ_GET_DESCR, USB_REQUEST_GET_DESCRIPTOR, index, USB_DESCRIPTOR_STRING, langid, ns, ns, dataptr, nullptr);
}

// Set address
uint8_t USB::setAddr(uint8_t oldaddr, uint8_t ep, uint8_t newaddr) {
  uint8_t rcode = ctrlReq(oldaddr, ep, bmREQ_SET, USB_REQUEST_SET_ADDRESS, newaddr, 0x00, 0x0000, 0x0000, 0x0000, nullptr, nullptr);
  //delay(2); // Per USB 2.0 sect.9.2.6.3
  delay(300); // Older spec says you should wait at least 200ms
  return rcode;
  //return ctrlReq(oldaddr, ep, bmREQ_SET, USB_REQUEST_SET_ADDRESS, newaddr, 0x00, 0x0000, 0x0000, 0x0000, nullptr, nullptr);
}

// Set configuration
uint8_t USB::setConf(uint8_t addr, uint8_t ep, uint8_t conf_value) {
  return ctrlReq(addr, ep, bmREQ_SET, USB_REQUEST_SET_CONFIGURATION, conf_value, 0x00, 0x0000, 0x0000, 0x0000, nullptr, nullptr);
}

#endif // HAS_USB_FLASH_DRIVE && USE_UHS2_USB
