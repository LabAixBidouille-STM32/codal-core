#include "JDProtocol.h"
#include "CodalDmesg.h"
#include "Timer.h"

using namespace codal;

int JDLogicDriver::populateDriverInfo(JDDriver* driver, JDDriverInfo* info, uint8_t bytesRemaining)
{
    info->type = JD_DRIVER_INFO_TYPE_HELLO;
    info->size = 0;

    info->address = driver->device.address;
    info->flags = 0;

    if (driver->device.isPairing())
        info->flags |= JD_DRIVER_INFO_FLAGS_PAIRING_MODE;

    if (driver->device.isPaired())
        info->flags |= JD_DRIVER_INFO_FLAGS_PAIRED;

    if (driver->device.isPairable())
        info->flags |= JD_DRIVER_INFO_FLAGS_PAIRABLE;

    info->driver_class = driver->device.driver_class;

    if (bytesRemaining > 0)
    {
        int payloadSize = driver->populateDriverInfo(info, min(bytesRemaining, JD_DRIVER_INFO_MAX_PAYLOAD_SIZE));
        info->size = min(payloadSize, JD_DRIVER_INFO_MAX_PAYLOAD_SIZE);
    }

    info->error_code = driver->device.getError();

    if (info->error_code > 0)
        info->type = JD_DRIVER_INFO_TYPE_ERROR;

    return info->size + JD_DRIVER_INFO_HEADER_SIZE;
}

/**
 * Timer callback every 500 ms
 **/
void JDLogicDriver::timerCallback(Event)
{
    // no sense continuing if we dont have a bus to transmit on...
    if (!JDProtocol::instance || !JDProtocol::instance->bus.isRunning())
        return;

    txControlPacket->address = 0;

    JDControlPacket* cp = (JDControlPacket *)txControlPacket->data;
    cp->serial_number = target_get_serial();

    uint8_t dataOffset = 0;

    for (int i = 0; i < JD_PROTOCOL_DRIVER_ARRAY_SIZE; i++)
    {
        JDDriver* current = JDProtocol::instance->drivers[i];

        // ignore ourself
        if (current == NULL || current == this)
            continue;

        // compute the difference
        uint8_t difference = ((current->device.rolling_counter > this->rolling_counter) ? current->device.rolling_counter - this->rolling_counter : this->rolling_counter - current->device.rolling_counter);

        // if the driver is acting as a virtual driver, we don't need to perform any initialisation, just connect / disconnect events.
        if (current->device.flags & JD_DEVICE_FLAGS_REMOTE)
        {
            if (!(current->device.flags & JD_DEVICE_FLAGS_CP_SEEN) && difference > 2)
            {
                JD_DMESG("CONTROL NOT SEEN %d %d", current->device.address, current->device.serial_number);
                current->deviceRemoved();
                Event(this->id, JD_LOGIC_DRIVER_EVT_CHANGED);

                if (current->device.flags & JD_DEVICE_FLAGS_BROADCAST)
                {
                    JD_DMESG("BROADCAST REM %d", current->device.address);
                    JDProtocol::instance->remove(*current);
                    delete current;
                    continue;
                }
            }
            else
                current->device.rolling_counter = this->rolling_counter;

            current->device.flags &= ~(JD_DEVICE_FLAGS_CP_SEEN);
            continue;
        }

        // local drivers run on the device
        if (current->device.flags & JD_DEVICE_FLAGS_LOCAL)
        {
            JDDriverInfo* info = (JDDriverInfo *)(cp->data + dataOffset);

            // initialise a driver by queuing a control packet with a first reasonable address
            if (!(current->device.flags & (JD_DEVICE_FLAGS_INITIALISED | JD_DEVICE_FLAGS_INITIALISING)))
            {
                JD_DMESG("BEGIN INIT");
                current->device.address = 0;

                bool allocated = true;

                // compute a reasonable first address
                while(allocated)
                {
                    bool stillAllocated = false;
                    current->device.address = target_random(256);

                    for (int j = 0; j < JD_PROTOCOL_DRIVER_ARRAY_SIZE; j++)
                    {
                        if (i == j)
                            continue;

                        if (JDProtocol::instance->drivers[j] && JDProtocol::instance->drivers[j]->device.flags & JD_DEVICE_FLAGS_INITIALISED)
                        {
                            if (JDProtocol::instance->drivers[j]->device.address == current->device.address)
                            {
                                stillAllocated = true;
                                break;
                            }
                        }
                    }

                    allocated = stillAllocated;
                }

                JD_DMESG("ALLOC: %d",current->device.address);

                // flag our address as uncertain (i.e. not committed / finalised)
                dataOffset += populateDriverInfo(current, info, 0);
                info->flags = JD_DRIVER_INFO_FLAGS_UNCERTAIN;
                current->device.flags |= JD_DEVICE_FLAGS_INITIALISING;
                current->device.rolling_counter = this->rolling_counter;
            }
            else if(current->device.flags & JD_DEVICE_FLAGS_INITIALISING)
            {
                // if no one has complained in a second, consider our address allocated
                // what happens if two devices do this? need to track rolling counter when it was set
                dataOffset += populateDriverInfo(current, info, 0);

                if (difference == 2)
                {
                    JD_DMESG("FINISHED");
                    current->device.flags &= ~JD_DEVICE_FLAGS_INITIALISING;
                    current->device.flags |= JD_DEVICE_FLAGS_INITIALISED;
                    current->deviceConnected(current->device);
                    Event(this->id, JD_LOGIC_DRIVER_EVT_CHANGED);
                }
                JD_DMESG("DIFFERENCE %d ", difference);
            }
            else if (current->device.flags & JD_DEVICE_FLAGS_INITIALISED)
                dataOffset += populateDriverInfo(current, info, JD_SERIAL_MAX_DATA_SIZE - dataOffset);
        }
    }

    // only transmit if we have DriverInformation to send.
    if (dataOffset > 0)
    {
        txControlPacket->size = JD_CONTROL_PACKET_HEADER_SIZE + dataOffset;
        JDProtocol::send(txControlPacket);
    }

    this->rolling_counter++;
}

JDLogicDriver::JDLogicDriver() : JDDriver(JDDevice(0, JD_DEVICE_FLAGS_LOCAL | JD_DEVICE_FLAGS_INITIALISED, 0, 0))
{
    this->device.address = 0;

    this->rxControlPacket = (JDControlPacket *)malloc(sizeof(JDControlPacket) + sizeof(JDDriverInfo) + JD_DRIVER_INFO_MAX_PAYLOAD_SIZE);
    this->txControlPacket = (JDPacket *)malloc(sizeof(JDPacket));

    status = 0;
    memset(this->address_filters, 0, JD_LOGIC_DRIVER_MAX_FILTERS);
    status |= (DEVICE_COMPONENT_RUNNING);

    if (EventModel::defaultEventBus)
    {
        EventModel::defaultEventBus->listen(this->id, JD_LOGIC_DRIVER_EVT_TIMER_CALLBACK, this, &JDLogicDriver::timerCallback);
        system_timer_event_every(500, this->id, JD_LOGIC_DRIVER_EVT_TIMER_CALLBACK);
    }
}

int JDLogicDriver::handleControlPacket(JDControlPacket* p)
{
    // nop for now... could be useful in the future for controlling the mode of the logic driver?
    return DEVICE_OK;
}

/**
  * Given a control packet, finds the associated driver, or if no associated device, associates a remote device with a driver.
  **/
int JDLogicDriver::handlePacket(JDPacket* pkt)
{
    JDControlPacket *cp = (JDControlPacket *)pkt->data;

    // incompatible version
    if (JD_SERIAL_PACKET_GET_VERSION(pkt) != JD_VERSION)
        return DEVICE_OK;

    uint8_t* dataPointer = cp->data;

    // set the serial_number of our statically allocated rx control packet (which is given to every driver)
    rxControlPacket->serial_number = cp->serial_number;

    JD_DMESG("CP size:%d wh:%d", pkt->size, (pkt->size - JD_CONTROL_PACKET_HEADER_SIZE));

    while (dataPointer < cp->data + (pkt->size - JD_CONTROL_PACKET_HEADER_SIZE))
    {
        JDDriverInfo* driverInfo = (JDDriverInfo *)dataPointer;

        // presumably we will eventually pass this control packet to a driver.
        // copy the driver information into our statically allocated control packet.
        if (driverInfo->size > 0 && driverInfo->size <= JD_DRIVER_INFO_MAX_PAYLOAD_SIZE)
            memcpy(rxControlPacket->data, driverInfo, JD_DRIVER_INFO_HEADER_SIZE + driverInfo->size);

        // special packet types should be handled here.
        if (driverInfo->type == JD_DRIVER_INFO_TYPE_PANIC)
        {
            uint8_t* errorName = (uint8_t*)driverInfo->data;

            if (driverInfo->size > 0)
            {
                int len = min(JD_CONTROL_PACKET_ERROR_NAME_LENGTH, driverInfo->size);
                char name[JD_CONTROL_PACKET_ERROR_NAME_LENGTH + 1] = { 0 };
                memcpy(name, errorName, len);
                name[len] = 0;

                JD_DMESG("%s is panicking [%d]", name, driverInfo->code);
            }

            dataPointer += JD_DRIVER_INFO_HEADER_SIZE + driverInfo->size;
            continue;
        }

        JD_DMESG("DI A:%d S:%d C:%d p: %d", driverInfo->address, cp->serial_number, driverInfo->driver_class, (driverInfo->flags & JD_DRIVER_INFO_FLAGS_PAIRING_MODE) ? 1 : 0);

        // Logic Driver addressing rules:
        // 1. drivers cannot have the same address and different serial numbers.
        // 2. if someone has flagged a conflict with you, you must reassign your address.

        // Address assignment rules:
        // 1. if you are initialising (address unconfirmed), set JD_DRIVER_INFO_FLAGS_UNCERTAIN
        // 2. if an existing, confirmed device spots a packet with the same address and the uncertain flag set, it should respond with
        //    the same packet, with the CONFLICT flag set.
        // 2b. if the transmitting device has no uncertain flag set, we reassign ourselves (first CP wins)
        // 3. upon receiving a packet with the conflict packet set, the receiving device should reassign their address.

        // first check for any drivers who are associated with this control packet
        bool handled = false; // indicates if the control packet has been handled by a driver.

        // we use this variable to determine if a new broadcast map needs to be created.
        bool representation_required = true;

        // devices about to enter pairing mode enumerate themselves, so that they have an address on the bus.
        // devices with uncertain addresses cannot be used
        // These two scenarios mean that drivers in this state are unusable, so we determine their packets as unsafe... "dropping" their packets
        bool safe = (driverInfo->flags & (JD_DRIVER_INFO_FLAGS_UNCERTAIN | JD_DRIVER_INFO_FLAGS_PAIRING_MODE)) == 0; // the packet it is safe

        for (int i = 0; i < JD_PROTOCOL_DRIVER_ARRAY_SIZE; i++)
        {
            JDDriver* current = JDProtocol::instance->drivers[i];

            if (current == NULL || current->device.driver_class != driverInfo->driver_class)
                continue;

            bool address_check = current->device.address == driverInfo->address;
            // bool class_check = true; // unused
            bool serial_check = cp->serial_number == current->device.serial_number;
            // this boolean is used to override stringent address checks (not needed for broadcast drivers as they receive all packets) to prevent code duplication
            bool broadcast_override = (current->device.flags & JD_DEVICE_FLAGS_BROADCAST) ? true : false;

            JD_DMESG("d a %d, s %d, c %d, i %d, t %c%c%c", current->device.address, current->device.serial_number, current->device.driver_class, current->device.flags & JD_DEVICE_FLAGS_INITIALISED ? 1 : 0, current->device.flags & JD_DEVICE_FLAGS_BROADCAST ? 'B' : ' ', current->device.flags & JD_DEVICE_FLAGS_LOCAL ? 'L' : ' ', current->device.flags & JD_DEVICE_FLAGS_REMOTE ? 'R' : ' ');
            if (address_check)
                representation_required = false;

            // We are in charge of local drivers, in this if statement we handle address assignment
            if ((address_check || broadcast_override) && current->device.flags & JD_DEVICE_FLAGS_INITIALISED)
            {
                JD_DMESG("ADDR MATCH");
                if (current->device.flags & JD_DEVICE_FLAGS_LOCAL)
                {
                    // a different device is using our address!!
                    if (!serial_check && address_check && !(driverInfo->flags & JD_DRIVER_INFO_FLAGS_CONFLICT))
                    {
                        JD_DMESG("SERIAL_DIFF");
                        // if we're initialised, this means that someone else is about to use our address, reject.
                        // see 2. above.
                        if ((current->device.flags & JD_DEVICE_FLAGS_INITIALISED) && (driverInfo->flags & JD_DRIVER_INFO_FLAGS_UNCERTAIN))
                        {
                            memcpy(rxControlPacket->data, driverInfo, JD_DRIVER_INFO_HEADER_SIZE);
                            JDDriverInfo* di = (JDDriverInfo*) cp->data;
                            di->flags |= JD_DRIVER_INFO_FLAGS_CONFLICT;
                            di->size = 0;
                            send((uint8_t*)rxControlPacket, JD_CONTROL_PACKET_HEADER_SIZE + JD_CONTROL_PACKET_HEADER_SIZE);
                            JD_DMESG("ASK OTHER TO REASSIGN");
                        }
                        // the other device is initialised and has transmitted the CP first, we lose.
                        else
                        {
                            // new address will be assigned on next tick.
                            current->device.address = 0;
                            current->device.flags &= ~(JD_DEVICE_FLAGS_INITIALISING | JD_DEVICE_FLAGS_INITIALISED);
                            JD_DMESG("INIT REASSIGNING SELF");
                        }

                        continue;
                    }
                    // someone has flagged a conflict with this initialised device
                    else if (driverInfo->flags & JD_DRIVER_INFO_FLAGS_CONFLICT)
                    {
                        // new address will be assigned on next tick.
                        current->deviceRemoved();
                        Event(this->id, JD_LOGIC_DRIVER_EVT_CHANGED);
                        JD_DMESG("REASSIGNING SELF");
                        continue;
                    }

                    // if we get here it means that:
                        // 1) address is the same as we expect
                        // 2) the serial_number is the same as we expect
                        // 3) we are not conflicting with another device.
                        // 4) someone external has addressed a packet to us.
                    JD_DMESG("FOUND LOCAL");

                    current->device.setBaudRate((JDBaudRate)pkt->communication_rate);
                    if (safe && current->handleLogicPacket(rxControlPacket) == DEVICE_OK)
                    {
                        handled = true;
                        JD_DMESG("L ABSORBED %d", current->device.address);
                        continue;
                    }
                }

                // for remote drivers, we aren't in charge, keep track of our remote...
                else if (current->device.flags & JD_DEVICE_FLAGS_REMOTE)
                {
                    // if the serial numbers differ, but the address is the same, it means that our original remote has gone...
                    // uninitialise.
                    if (!serial_check)
                    {
                        current->deviceRemoved();
                        continue;
                    }
                    else
                    {
                        // all is good, flag the device so that it is not removed.
                        current->device.setBaudRate((JDBaudRate)pkt->communication_rate);
                        current->device.flags |= JD_DEVICE_FLAGS_CP_SEEN;
                        JD_DMESG("FOUND REMOTE a:%d sn:%d i:%d", current->device.address, current->device.serial_number, current->device.flags & JD_DEVICE_FLAGS_INITIALISED ? 1 : 0);

                        if (safe && current->handleLogicPacket(rxControlPacket) == DEVICE_OK)
                        {
                            handled = true;
                            JD_DMESG("R ABSORBED %d", current->device.address);
                            continue;
                        }
                    }
                }
            }
        }

        JD_DMESG("OUT: hand %d safe %d", handled, safe);

        if (handled || !safe)
            JD_DMESG("HANDLED");
        else
        {
            bool filtered = filterPacket(driverInfo->address);

            // if it's paired with a driver and it's not us, we can just ignore
            if (!filtered && driverInfo->flags & JD_DRIVER_INFO_FLAGS_PAIRED)
                addToFilter(driverInfo->address);

            // if it was previously paired with another device, we remove the filter.
            else if (filtered && !(driverInfo->flags & JD_DRIVER_INFO_FLAGS_PAIRED))
                removeFromFilter(driverInfo->address);

            else
            {
                JD_DMESG("SEARCH");
                bool found = false;

                // if we reach here, there is no associated device, find a free remote instance in the drivers array
                for (int i = 0; i < JD_PROTOCOL_DRIVER_ARRAY_SIZE; i++)
                {
                    JDDriver* current = JDProtocol::instance->drivers[i];
                    // JD_DMESG("FIND DRIVER");
                    if (current && current->device.flags & JD_DEVICE_FLAGS_REMOTE && !(current->device.flags & JD_DEVICE_FLAGS_INITIALISED) && current->device.driver_class == driverInfo->driver_class)
                    {
                        JD_DMESG("ITER a %d, s %d, c %d, t %c%c%c", current->device.address, current->device.serial_number, current->device.driver_class, current->device.flags & JD_DEVICE_FLAGS_BROADCAST ? 'B' : ' ', current->device.flags & JD_DEVICE_FLAGS_LOCAL ? 'L' : ' ', current->device.flags & JD_DEVICE_FLAGS_REMOTE ? 'R' : ' ');
                        // this driver instance is looking for a specific serial number
                        if (current->device.serial_number > 0 && current->device.serial_number != cp->serial_number)
                            continue;

                        JD_DMESG("FOUND NEW: %d %d %d", current->device.address, current->device.driver_class);
                        current->device.setBaudRate((JDBaudRate)pkt->communication_rate);
                        int ret = current->handleLogicPacket(rxControlPacket);

                        if (ret == DEVICE_OK)
                        {
                            current->deviceConnected(JDDevice(driverInfo->address, driverInfo->flags, cp->serial_number, driverInfo->driver_class));
                            Event(this->id, JD_LOGIC_DRIVER_EVT_CHANGED);
                            found = true;
                            break;
                        }
                        // keep going if the driver has returned DEVICE_CANCELLED.
                    }
                }

                // only add a broadcast device if it is not already represented in the driver array.
                // we all need good representation, which is apparently very hard in the real world, lets try our best in software ;)
                if (representation_required && !found)
                {
                    JD_DMESG("ADD NEW MAP");
                    JD_DMESG("BROADCAST ADD %d", driverInfo->address);
                    new JDDriver(JDDevice(driverInfo->address, JD_DEVICE_FLAGS_BROADCAST | JD_DEVICE_FLAGS_REMOTE | JD_DEVICE_FLAGS_INITIALISED | JD_DEVICE_FLAGS_CP_SEEN, cp->serial_number, driverInfo->driver_class));
                    Event(this->id, JD_LOGIC_DRIVER_EVT_CHANGED);
                }
            }
        }

        dataPointer += JD_DRIVER_INFO_HEADER_SIZE + driverInfo->size;
    }

    return DEVICE_OK;
}

int JDLogicDriver::addToFilter(uint8_t address)
{
    JD_DMESG("FILTER: %d", address);
    // we shouldn't filter any addresses that we are virtualising or hosting.
    for (int i = 0; i < JD_PROTOCOL_DRIVER_ARRAY_SIZE; i++)
    {
        if (address == JDProtocol::instance->drivers[i]->getAddress())
            return DEVICE_OK;
    }

    for (int i = 0; i < JD_LOGIC_DRIVER_MAX_FILTERS; i++)
    {
        if (this->address_filters[i] == 0)
            this->address_filters[i] = address;
    }

    return DEVICE_OK;
}

int JDLogicDriver::removeFromFilter(uint8_t address)
{
    JD_DMESG("UNFILTER: %d", address);
    for (int i = 0; i < JD_LOGIC_DRIVER_MAX_FILTERS; i++)
    {
        if (this->address_filters[i] == address)
            this->address_filters[i] = 0;
    }

    return DEVICE_OK;
}

bool JDLogicDriver::filterPacket(uint8_t address)
{
    if (address > 0)
    {
        for (int i = 0; i < JD_PROTOCOL_DRIVER_ARRAY_SIZE; i++)
            if (address_filters[i] == address)
                return true;
    }

    return false;
}