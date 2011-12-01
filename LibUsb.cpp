/*
 * Copyright (c) 2011 Darren Hague & Eric Brandt
 *               Modified to suport Linux and OSX by Mark Liversedge
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "LibUsb.h"

LibUsb::LibUsb()
{
    // dynamic load of libusb on Windows, it is statically linked in Linux
    // this is to avoid dll conflicts where the lib has already been installed
    this->intf = NULL;
    this->device = NULL;
    usb_set_debug(0);
}

int LibUsb::open()
{

   // Initialize the library.
    usb_init();

    // Find all busses.
    usb_find_busses();

    // Find all connected devices.
    usb_find_devices();

    // Search USB busses for USB2 ANT+ stick host controllers
    device = OpenAntStick();

    if (device == NULL) return -1;

    int rc;

#ifndef Q_OS_MAC
    // these functions fail on OS X Lion
    rc = usb_clear_halt(device, writeEndpoint);
    if (rc < 0) qCritical()<<"usb_clear_halt writeEndpoint Error: "<< usb_strerror();

    rc = usb_clear_halt(device, readEndpoint);
    if (rc < 0) qCritical()<<"usb_clear_halt readEndpoint Error: "<< usb_strerror();
#endif

    return rc;
}

void LibUsb::close()
{
    if (this->device) {
        // stop any further write attempts whilst we close down
        qDebug() << "Closing USB connection...";
        usb_dev_handle *p = this->device;
        this->device = NULL;

        usb_release_interface(p, interface);
        usb_close(p);
    }
}

int LibUsb::read(QByteArray *buf, int bytes)
{

    // check it isn't closed already
    if (!device) return -1;

    char buffer[bytes];
    int rc = usb_bulk_read(this->device, this->readEndpoint, buffer, bytes, USB_TIMEOUT_MSEC);
    qDebug() << "Bytes read: " << rc;

    // we clear the buffer.
    buf->clear();
    QString data, s;

    if (rc > 0) {
        for (quint32 i = 0; i < sizeof(buffer); i++) {
            buf->append(buffer[i]);
            data.append(s.sprintf("%02X",(uchar)buf->at(i))+":");
        }
        data.remove(data.size()-1, 1); //remove last colon
        qDebug() << "Received: " << data;
    }

    if (rc < 0)
    {
        if (rc == -110)
            qDebug() << "Timeout";
        else
            qCritical() << "usb_bulk_read Error reading: " << rc << usb_strerror();
        return rc;
    }

    return rc;
}

int LibUsb::write(QByteArray *buf, int bytes)
{

    // check it isn't closed
    if (!this->device) return -1;

    QString cmd, s;
        for (int i=0; i<buf->size(); i++) {
            cmd.append(s.sprintf("%02X",(uchar)buf->at(i))+":");
        }
        cmd.remove(cmd.size()-1, 1); //remove last colon
        qDebug() << "Sending" << buf->size() << "bytes:" << cmd;

    // we use a non-interrupted write on Linux/Mac since the interrupt
    // write block size is incorectly implemented in the version of
    // libusb we build with. It is no less efficent.
    int rc = usb_bulk_write(this->device, this->writeEndpoint, buf->constData(), bytes, USB_TIMEOUT_MSEC);

    if (rc < 0)
    {
        if (rc == -110)
            qDebug() << "Timeout";
        else if (rc == -2)
            qCritical() << "EndPoint not found";
        else
            qCritical() << "usb_interrupt_write Error writing: "<< usb_strerror();
    }

    return rc;
}

struct usb_dev_handle* LibUsb::OpenAntStick()
{

    struct usb_bus* bus;
    struct usb_device* dev;
    struct usb_dev_handle* udev;

    for (bus = usb_get_busses(); bus; bus = bus->next) {

        for (dev = bus->devices; dev; dev = dev->next) {

            if (dev->descriptor.idVendor == USB_ST_VID && dev->descriptor.idProduct == USB_STLINK_PID) {

                qCritical() << "Found ST an Link V1, this one is not supported!";
                return NULL;
            }

            else if (dev->descriptor.idVendor == USB_ST_VID && dev->descriptor.idProduct == USB_STLINKv2_PID) {

                //Avoid noisy output
                qInformal() << "Found an ST Link V2.";

                if ((udev = usb_open(dev))) {
                    qInformal() << "Opening device...";

                    if (dev->descriptor.bNumConfigurations) {

                        if ((intf = usb_find_interface(&dev->config[0])) != NULL) { // Loading first config.

                            int rc = usb_set_configuration(udev, 1);
                            if (rc < 0) {
                                qCritical()<<"usb_set_configuration Error: "<< usb_strerror();
#ifdef __linux__
                                // looks like the udev rule has not been implemented
                                qCritical()<<"check permissions on:"<<QString("/dev/bus/usb/%1/%2").arg(bus->dirname).arg(dev->filename);
                                qCritical()<<"did you remember to setup a udev rule for this device?";
#endif
                            }

                            rc = usb_claim_interface(udev, this->interface);
                            if (rc < 0) qCritical()<<"usb_claim_interface Error: "<< usb_strerror();

//#ifndef Q_OS_MAC
//                            // fails on Mac OS X, we don't actually need it anyway
//                            rc = usb_set_altinterface(udev, alternate);
//                            if (rc < 0) qDebug()<<"usb_set_altinterface Error: "<< usb_strerror();
//#endif
                            qInformal() << "Device Open.";
                            return udev;
                        }
                        else qCritical() << "Could not load interface configuration.";
                    }

                    usb_close(udev);
                }
            }
        }
    }
    qCritical() << "Found nothing...";
    return NULL;
}

struct usb_interface_descriptor* LibUsb::usb_find_interface(struct usb_config_descriptor* config_descriptor)
{

    struct usb_interface_descriptor* intf;

    this->readEndpoint = 0;
    this->writeEndpoint = 0;
    this->interface = 0;
    this->alternate = 0;

    if (!config_descriptor) return NULL;

    if (!config_descriptor->bNumInterfaces) return NULL;

    if (!config_descriptor->interface[0].num_altsetting) return NULL;

    intf = &config_descriptor->interface[0].altsetting[0];

    this->interface = intf->bInterfaceNumber;
    this->alternate = intf->bAlternateSetting;

    this->readEndpoint = USB_PIPE_IN; // IN = ST link -> Host
    this->writeEndpoint = USB_PIPE_OUT; // OUT = Host -> ST link

    if (this->readEndpoint < 0 || this->writeEndpoint < 0)
        return NULL;

    return intf;
}
