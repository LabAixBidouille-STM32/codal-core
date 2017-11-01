/*
The MIT License (MIT)

Copyright (c) 2017 Lancaster University.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
*/

/* Based on: */

/*
             LUFA Library
     Copyright (C) Dean Camera, 2017.

  dean [at] fourwalledcubicle [dot] com
           www.lufa-lib.org
*/

/*
  Copyright 2017  Dean Camera (dean [at] fourwalledcubicle [dot] com)

  Permission to use, copy, modify, distribute, and sell this
  software and its documentation for any purpose is hereby granted
  without fee, provided that the above copyright notice appear in
  all copies and that both that the copyright notice and this
  permission notice and warranty disclaimer appear in supporting
  documentation, and that the name of the author not be used in
  advertising or publicity pertaining to distribution of the
  software without specific, written prior permission.

  The author disclaims all warranties with regard to this
  software, including all implied warranties of merchantability
  and fitness.  In no event shall the author be liable for any
  special, indirect or consequential damages or any damages
  whatsoever resulting from loss of use, data or profits, whether
  in an action of contract, negligence or other tortious action,
  arising out of or in connection with the use or performance of
  this software.
*/

#include "USBMSC.h"

#if CONFIG_ENABLED(DEVICE_USB)

#include "lufa/MassStorageClassCommon.h"

#define CPU_TO_LE32(x) (x)
#define le32_to_cpu(x) (x)

#define SCSI_SET_SENSE(Key, Acode, Aqual)                                                          \
    do                                                                                             \
    {                                                                                              \
        state->SenseData.SenseKey = (Key);                                                         \
        state->SenseData.AdditionalSenseCode = (Acode);                                            \
        state->SenseData.AdditionalSenseQualifier = (Aqual);                                       \
    } while (0)

static inline uint32_t CPU_TO_BE32(uint32_t v)
{
    return (v << 24) | ((v & 0xff00) << 8) | ((v & 0xff0000) >> 8) | (v >> 24);
}

static inline uint16_t read16(uint8_t *p)
{
    return (p[0] << 8) | p[1];
}

static inline uint16_t read32(uint8_t *p)
{
    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

namespace codal
{
struct MSCState
{
    MS_CommandBlockWrapper_t CommandBlock;
    MS_CommandStatusWrapper_t CommandStatus;
    SCSI_Request_Sense_Response_t SenseData;
};
}

using namespace codal;

static const InterfaceInfo ifaceInfo = {
    NULL, // No supplemental descriptor
    0,    // ditto
    2,    // two endpoints
    {
        2,    // numEndpoints
        0x08, // class code - mass storage
        0x06, // subclass - SCSI transparent command set
        0x80, // protocol code - bulk only transport
        0x00, // iface string
        0x00, // alt setting
    },
    {USB_EP_TYPE_BULK, 0},
    {USB_EP_TYPE_BULK, 0},
};

int USBMSC::classRequest(UsbEndpointIn &ctrl, USBSetup &setup)
{
    uint8_t tmp;

    switch (setup.bRequest)
    {
    case MS_REQ_GetMaxLUN:
        tmp = totalLUNs() - 1;
        return ctrl.write(&tmp, 1);
    }

    return DEVICE_NOT_SUPPORTED;
}

const InterfaceInfo *USBMSC::getInterfaceInfo()
{
    return &ifaceInfo;
}

int USBMSC::endpointRequest()
{
    int len = out->read(&state->CommandBlock, sizeof(state->CommandBlock));

    if (len != sizeof(state->CommandBlock))
        goto stall;

    if ((state->CommandBlock.Signature != CPU_TO_LE32(MS_CBW_SIGNATURE)) ||
        (state->CommandBlock.LUN >= totalLUNs()) || (state->CommandBlock.Flags & 0x1F) ||
        (state->CommandBlock.SCSICommandLength == 0) ||
        (state->CommandBlock.SCSICommandLength > 16))
        goto stall;

    return handeSCSICommand();

stall:
    out->stall();
    in->stall();
    return 0;
}

USBMSC::USBMSC() : CodalUSBInterface()
{
    state = new MSCState();
    memset(state, 0, sizeof(*state));
    state->SenseData.ResponseCode = 0x70;
    state->SenseData.AdditionalLength = 0x0A;
}

int USBMSC::sendResponse(bool ok)
{
    if (ok)
    {
        SCSI_SET_SENSE(SCSI_SENSE_KEY_GOOD, SCSI_ASENSE_NO_ADDITIONAL_INFORMATION,
                       SCSI_ASENSEQ_NO_QUALIFIER);
    }

    state->CommandStatus.Status = ok ? MS_SCSI_COMMAND_Pass : MS_SCSI_COMMAND_Fail;
    state->CommandStatus.Signature = CPU_TO_LE32(MS_CSW_SIGNATURE);
    state->CommandStatus.Tag = state->CommandBlock.Tag;
    state->CommandStatus.DataTransferResidue = state->CommandBlock.DataTransferLength;

    if (!ok && (le32_to_cpu(state->CommandStatus.DataTransferResidue)))
    {
        if (state->CommandBlock.Flags & MS_COMMAND_DIR_DATA_IN)
            in->stall();
        else
            out->stall();
    }

    // TODO wait for stall clear?

    if (!writePadded(&state->CommandStatus, sizeof(state->CommandStatus),
                     sizeof(state->CommandStatus)))
        return -1;
    return 0;
}

int USBMSC::handeSCSICommand()
{
    bool ok = false;

    /* Run the appropriate SCSI command hander function based on the passed command */
    switch (state->CommandBlock.SCSICommandData[0])
    {
    case SCSI_CMD_INQUIRY:
        ok = cmdInquiry();
        break;
    case SCSI_CMD_REQUEST_SENSE:
        ok = cmdRequest_Sense();
        break;
    case SCSI_CMD_READ_CAPACITY_10:
        ok = cmdRead_Capacity_10();
        break;
    case SCSI_CMD_SEND_DIAGNOSTIC:
        ok = cmdSend_Diagnostic();
        break;
    case SCSI_CMD_WRITE_10:
        cmdReadWrite_10(false);
        return 0; // sends response by itself
    case SCSI_CMD_READ_10:
        cmdReadWrite_10(true);
        return 0; // ditto
    case SCSI_CMD_MODE_SENSE_6:
        ok = cmdModeSense_6();
        break;
    case SCSI_CMD_START_STOP_UNIT:
    case SCSI_CMD_TEST_UNIT_READY:
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
    case SCSI_CMD_VERIFY_10:
        /* These commands should just succeed, no handling required */
        ok = true;
        state->CommandBlock.DataTransferLength = 0;
        break;
    default:
        /* Update the SENSE key to reflect the invalid command */
        SCSI_SET_SENSE(SCSI_SENSE_KEY_ILLEGAL_REQUEST, SCSI_ASENSE_INVALID_COMMAND,
                       SCSI_ASENSEQ_NO_QUALIFIER);
        break;
    }

    return sendResponse(ok);
}

void USBMSC::readBulk(void *ptr, int dataSize)
{
    usb_assert(dataSize % 64 == 0);
    if (failed)
    {
        memset(ptr, 0, dataSize);
        return;
    }
    state->CommandBlock.DataTransferLength -= dataSize;
    if (out->read(ptr, dataSize) < 0)
        fail();
}

void USBMSC::fail()
{
    failed = true;
}

void USBMSC::writeBulk(const void *ptr, int dataSize)
{
    usb_assert(dataSize % 64 == 0);
    if (failed)
        return;
    state->CommandBlock.DataTransferLength -= dataSize;
    in->flags |= USB_EP_FLAG_NO_AUTO_ZLP; // disable AUTO-ZLP
    if (in->write(ptr, dataSize) < 0)
        fail();
}

bool USBMSC::writePadded(const void *ptr, int dataSize, int allocSize)
{
    in->flags &= ~USB_EP_FLAG_NO_AUTO_ZLP; // enable AUTO-ZLP

    if (dataSize >= allocSize)
    {
        if (in->write(ptr, allocSize) < 0)
            return false;
        dataSize = allocSize;
    }
    else
    {
        uint8_t tmp[allocSize];
        memset(tmp, 0, allocSize);
        memcpy(tmp, ptr, dataSize);
        if (in->write(tmp, allocSize) < 0)
            return false;
    }

    state->CommandBlock.DataTransferLength -= dataSize;
    return true;
}

bool USBMSC::cmdInquiry()
{
    SCSI_Inquiry_Response_t InquiryData;

    uint16_t alloc = read16(&state->CommandBlock.SCSICommandData[3]);

    memset(&InquiryData, 0, sizeof(InquiryData));

    InquiryData.Removable = true;
    InquiryData.ResponseDataFormat = 2;
    InquiryData.AdditionalLength = 0x1F;

    //.VendorID            = "LUFA",
    //.ProductID           = "Dataflash Disk",
    memcpy(InquiryData.RevisionID, "1.00", 4);

    /* Only the standard INQUIRY data is supported, check if any optional INQUIRY bits set */
    if ((state->CommandBlock.SCSICommandData[1] & ((1 << 0) | (1 << 1))) ||
        state->CommandBlock.SCSICommandData[2])
    {
        /* Optional but unsupported bits set - update the SENSE key and fail the request */
        SCSI_SET_SENSE(SCSI_SENSE_KEY_ILLEGAL_REQUEST, SCSI_ASENSE_INVALID_FIELD_IN_CDB,
                       SCSI_ASENSEQ_NO_QUALIFIER);

        return false;
    }

    return writePadded(&InquiryData, sizeof(InquiryData), alloc);
}

bool USBMSC::cmdRequest_Sense()
{
    uint8_t alloc = state->CommandBlock.SCSICommandData[4];
    return writePadded(&state->SenseData, sizeof(state->SenseData), alloc);
}

bool USBMSC::cmdRead_Capacity_10()
{
    uint32_t info[] = {CPU_TO_BE32(getCapacity() - 1), // final address
                       CPU_TO_BE32(512)};

    return writePadded(&info, sizeof(info), 8);
}

bool USBMSC::cmdSend_Diagnostic()
{
    /* Check to see if the SELF TEST bit is not set */
    if (!(state->CommandBlock.SCSICommandData[1] & (1 << 2)))
    {
        /* Only self-test supported - update SENSE key and fail the command */
        SCSI_SET_SENSE(SCSI_SENSE_KEY_ILLEGAL_REQUEST, SCSI_ASENSE_INVALID_FIELD_IN_CDB,
                       SCSI_ASENSEQ_NO_QUALIFIER);

        return false;
    }

    /* Check to see if all attached Dataflash ICs are functional */
    if (!storageOK())
    {
        /* Update SENSE key with a hardware error condition and return command fail */
        SCSI_SET_SENSE(SCSI_SENSE_KEY_HARDWARE_ERROR, SCSI_ASENSE_NO_ADDITIONAL_INFORMATION,
                       SCSI_ASENSEQ_NO_QUALIFIER);

        return false;
    }

    /* Succeed the command and update the bytes transferred counter */
    state->CommandBlock.DataTransferLength = 0;

    return true;
}

void USBMSC::finishReadWrite()
{
    bool ok = !failed;
    failed = false;
    sendResponse(ok);
}

void USBMSC::cmdReadWrite_10(bool isRead)
{
    uint32_t BlockAddress;
    uint16_t TotalBlocks;

    /* Check if the disk is write protected or not */
    if (!isRead && isReadOnly())
    {
        /* Block address is invalid, update SENSE key and return command fail */
        SCSI_SET_SENSE(SCSI_SENSE_KEY_DATA_PROTECT, SCSI_ASENSE_WRITE_PROTECTED,
                       SCSI_ASENSEQ_NO_QUALIFIER);
        sendResponse(false);
        return;
    }

    BlockAddress = read32(&state->CommandBlock.SCSICommandData[2]);
    TotalBlocks = read16(&state->CommandBlock.SCSICommandData[7]);

    /* Check if the block address is outside the maximum allowable value for the LUN */
    if (BlockAddress >= getCapacity())
    {
        /* Block address is invalid, update SENSE key and return command fail */
        SCSI_SET_SENSE(SCSI_SENSE_KEY_ILLEGAL_REQUEST,
                       SCSI_ASENSE_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE, SCSI_ASENSEQ_NO_QUALIFIER);
        sendResponse(false);
        return;
    }

    failed = false;
    if (isRead)
        readBlocks(BlockAddress, TotalBlocks);
    else
        writeBlocks(BlockAddress, TotalBlocks);
}

bool USBMSC::cmdModeSense_6()
{
    uint8_t ro = isReadOnly() ? 0x80 : 0x00;
    uint8_t resp[] = {0, 0, ro, 0};
    return writePadded(resp, sizeof(resp), 4);
}

int USBMSC::currLUN()
{
    return state->CommandBlock.LUN;
}

uint32_t USBMSC::cbwTag() {
    return state->CommandBlock.Tag;
}

#endif