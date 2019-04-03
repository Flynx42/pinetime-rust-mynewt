//  Ported from https://os.mbed.com/teams/ESP8266/code/esp8266-driver/file/6946b0b9e323/ESP8266/ESP8266.cpp/
/* ESP8266 Example
 * Copyright (c) 2015 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <console/console.h>
#include "ESP8266.h"

extern "C" int debug_vrecv;  ////

void ESP8266::init(char *txbuf, uint32_t txbuf_size, char *rxbuf, uint32_t rxbuf_size, 
    char *parserbuf, uint32_t parserbuf_size, bool debug)
{
    _uart = 0;
    _serial.init(txbuf, txbuf_size, rxbuf, rxbuf_size);
    _parser.init(_serial, parserbuf, parserbuf_size);
    _packets = 0;
    _packets_end = &_packets;
    _serial.baud(115200);
    _parser.debugOn(debug);
}

void ESP8266::configure(int uart) {
    _uart = uart;
    _serial.configure(uart);
}

void packet_handler(void *arg) {
    assert(arg != NULL);
    ((ESP8266 *)arg)->_packet_handler();
}

bool ESP8266::setEcho(bool echoEnabled) {
    //  Turn command echoing on or off.
    for (int i = 0; i < 2; i++) {  //  Try twice in case of error...
        if (
            _parser.send(       //  Send echo on or off command.
                echoEnabled 
                ? "\r\nATE1"
                : "\r\nATE0"
            ) &&
            _parser.recv("OK")  //  Wait for OK response.
        ) {
            console_printf("ESP setEcho OK\n"); console_flush(); return true; 
        }
    }
    console_printf("ESP setEcho FAILED\n"); console_flush(); 
    return false;
}

bool ESP8266::startup(int mode)
{
    //  only 3 valid modes
    if(mode < 1 || mode > 3) {
        return false;
    }
    bool success = 
        reset()            //  Restart the ESP8266 module.
        && setEcho(false)  //  Disable command echo because it complicates the response processing.
        && _parser.send("AT+CWMODE=%d", mode)  //  Set the mode to WiFi Client, WiFi Access Point, or both.
        && _parser.recv("OK")                  //  Wait for response.
        && _parser.send("AT+CIPMUX=1")         //  Allow multiple TCP/UDP connections.
        && _parser.recv("OK");                 //  Wait for response.
    _parser.oob("+IPD", packet_handler, this); //  Call the packet handler when network data is received.
    return success;
}

bool ESP8266::reset(void)
{
    for (int i = 0; i < 2; i++) {
#ifdef TOOO
        if (
            _parser.send("\r\nAT+RST") &&
            _parser.recv("OK\r\nready")  //  Wait response: "[System Ready, Vendor:www.ai-thinker.com]"
        ) {
            console_printf("ESP reset OK\n"); console_flush(); return true; 
        }
#endif  //  TODO
        if (
            _parser.send("\r\nAT+RST") &&
            _parser.recv("jump") &&  //  Wait for last line of response: "jump to run user1 @ 1000"
            _parser.recv("\r\n")     //  Wait for end of the line
        ) {
            console_printf("ESP reset OK\n"); console_flush(); return true; 
        }
    }
    console_printf("ESP reset FAILED\n"); console_flush(); 
    return false;
}

bool ESP8266::dhcp(bool enabled, int mode)
{
    //only 3 valid modes
    if(mode < 0 || mode > 2) {
        return false;
    }

    return _parser.send("AT+CWDHCP=%d,%d", enabled?1:0, mode)
        && _parser.recv("OK");
}

bool ESP8266::connect(const char *ap, const char *passPhrase)
{
    return _parser.send("AT+CWJAP=\"%s\",\"%s\"", ap, passPhrase)
        && _parser.recv("OK");
}

bool ESP8266::disconnect(void)
{
    return _parser.send("AT+CWQAP") && _parser.recv("OK");
}

const char *ESP8266::getIPAddress(void)
{
    if (!(_parser.send("AT+CIFSR")
        && _parser.recv("+CIFSR:STAIP,\"%15[^\"]\"", _ip_buffer)
        && _parser.recv("OK"))) {
        return 0;
    }

    return _ip_buffer;
}

const char *ESP8266::getMACAddress(void)
{
    if (!(_parser.send("AT+CIFSR")
        && _parser.recv("+CIFSR:STAMAC,\"%17[^\"]\"", _mac_buffer)
        && _parser.recv("OK"))) {
        return 0;
    }

    return _mac_buffer;
}

const char *ESP8266::getGateway()
{
    if (!(_parser.send("AT+CIPSTA?")
        && _parser.recv("+CIPSTA:gateway:\"%15[^\"]\"", _gateway_buffer)
        && _parser.recv("OK"))) {
        return 0;
    }

    return _gateway_buffer;
}

const char *ESP8266::getNetmask()
{
    if (!(_parser.send("AT+CIPSTA?")
        && _parser.recv("+CIPSTA:netmask:\"%15[^\"]\"", _netmask_buffer)
        && _parser.recv("OK"))) {
        return 0;
    }

    return _netmask_buffer;
}

int8_t ESP8266::getRSSI()
{
    int8_t rssi;
    char bssid[18];

   if (!(_parser.send("AT+CWJAP?")
        && _parser.recv("+CWJAP:\"%*[^\"]\",\"%17[^\"]\"", bssid)
        && _parser.recv("OK"))) {
        return 0;
    }

    if (!(_parser.send("AT+CWLAP=\"\",\"%s\",", bssid)
        && _parser.recv("+CWLAP:(%*d,\"%*[^\"]\",%hhd,", &rssi)
        && _parser.recv("OK"))) {
        return 0;
    }

    return rssi;
}

bool ESP8266::isConnected(void)
{
    return getIPAddress() != 0;
}

int ESP8266::scan(nsapi_wifi_ap_t *res, unsigned limit)
{
    unsigned cnt = 0;
    nsapi_wifi_ap_t ap;
    if (!_parser.send("AT+CWLAP")) {
        return NSAPI_ERROR_DEVICE_ERROR;
    }
    while (recv_ap(&ap)) {
        if (cnt < limit) {
            memcpy(&res[cnt], &ap, sizeof(ap));
        }
        cnt++;
        if (limit != 0 && cnt >= limit) {
            break;
        }
    }
    return cnt;
}

bool ESP8266::open(const char *type, int id, const char* addr, int port)
{
    //IDs only 0-4
    if(id > 4) {
        return false;
    }

    return _parser.send("AT+CIPSTART=%d,\"%s\",\"%s\",%d", id, type, addr, port)
        && _parser.recv("OK");
}

bool ESP8266::send(int id, const void *data, uint32_t amount)
{
    //May take a second try if device is busy
    for (unsigned i = 0; i < 2; i++) {
        if (_parser.send("AT+CIPSEND=%d,%d", id, amount)
            && _parser.recv(">")
            && _parser.write((char*)data, (int)amount) >= 0) {
            return true;
        }
    }

    return false;
}

void ESP8266::_packet_handler()
{
    int id;
    uint32_t amount;

    // parse out the packet
    if (!_parser.recv(",%d,%d:", &id, &amount)) {
        return;
    }

    struct packet *packet = (struct packet*)malloc(
            sizeof(struct packet) + amount);
    if (!packet) {
        return;
    }

    packet->id = id;
    packet->len = amount;
    packet->next = 0;

    if (!(_parser.read((char*)(packet + 1), amount))) {
        free(packet);
        return;
    }

    // append to packet list
    *_packets_end = packet;
    _packets_end = &packet->next;
}

int32_t ESP8266::recv(int id, void *data, uint32_t amount)
{
    while (true) {
        // check if any packets are ready for us
        for (struct packet **p = &_packets; *p; p = &(*p)->next) {
            if ((*p)->id == id) {
                struct packet *q = *p;

                if (q->len <= amount) { // Return and remove full packet
                    memcpy(data, q+1, q->len);

                    if (_packets_end == &(*p)->next) {
                        _packets_end = p;
                    }
                    *p = (*p)->next;

                    uint32_t len = q->len;
                    free(q);
                    return len;
                } else { // return only partial packet
                    memcpy(data, q+1, amount);

                    q->len -= amount;
                    memmove(q+1, (uint8_t*)(q+1) + amount, q->len);

                    return amount;
                }
            }
        }

        // Wait for inbound packet
        if (!_parser.recv("OK")) {
            return -1;
        }
    }
}

bool ESP8266::close(int id)
{
    //May take a second try if device is busy
    for (unsigned i = 0; i < 2; i++) {
        if (_parser.send("AT+CIPCLOSE=%d", id)
            && _parser.recv("OK")) {
            return true;
        }
    }

    return false;
}

void ESP8266::setTimeout(uint32_t timeout_ms)
{
    _parser.setTimeout(timeout_ms);
}

bool ESP8266::readable()
{
    return _serial.readable();
}

bool ESP8266::writeable()
{
    return _serial.writeable();
}

void ESP8266::attach(void (*func)(void *), void *arg)
{
    _serial.attach(func, arg);
}

bool ESP8266::recv_ap(nsapi_wifi_ap_t *ap)
{
    //  Parse the next line of WiFi AP info received, which looks like:
    //  +CWLAP:(3,"HP-Print-54-Officejet 0000",-74,"8c:dc:d4:00:00:00",1,-34,0)
    int sec = -1, channel = -1;
    memset(ap, 0, sizeof(nsapi_wifi_ap_t));
#ifdef NOTUSED
    int rc = sscanf(
        "+CWLAP:(3,\"HP-Print-54-Officejet 0000\",-74,\"8c:dc:d4:00:00:00\",1,-34,0)"
        //  ""
        ,
        "+CWLAP:(%d,\"%32[^\"]\",%hhd,\"%hhx:%hhx:%hhx:%hhx:%hhx:%hhx\",%d"
        //  ""
        , 
        &sec, ap->ssid, &ap->rssi, &ap->bssid[0], &ap->bssid[1], &ap->bssid[2], &ap->bssid[3], &ap->bssid[4], &ap->bssid[5], &channel
    );
    ap->channel = (uint8_t) channel;
    console_printf("sscanf %d\n", rc);
    return true;
#endif  //  NOTUSED
    //  Note: This parsing fails with the implementation of vsscanf() in Baselibc.  See vsscanf.c in this directory for the fixed implementation.
#ifdef NOTUSED
    bool ret = _parser.recv("+CWLAP:(%d,\"%32[^\"]\",%hhd,\"%hhx:%hhx:%hhx:%hhx:%hhx:%hhx\",%d", &sec, ap->ssid,
                            &ap->rssi, &ap->bssid[0], &ap->bssid[1], &ap->bssid[2], &ap->bssid[3], &ap->bssid[4],
                            &ap->bssid[5], &channel);  //  "&channel" was previously "&ap->channel", which is incorrect because "%d" assigns an int not uint8_t.
#endif  //  NOTUSED
    int count = -1;
    int rc = sscanf(
        "+CWLAP:(3,\"HP-Print-54-Officejet 0000\",-74,\"8c:dc:d4:00:00:00\",1,-34,0)"
        ,
        //  "+CWLAP:(%*d,\"%*32[^\"]\",%n"
        "+CWLAP:(%*d,\"%*32[^\"]\",%n"
        ,
        &count
    );
    console_printf("sscanf %d / %d\n", rc, count); console_flush();
    debug_vrecv = 1;  ////
    bool ret = _parser.recv("+CWLAP:(%d,\"%32[^\"]\","
                            //  "%hhd,\"%hhx:%hhx:%hhx:%hhx:%hhx:%hhx\",%d"
                            , &sec, ap->ssid
                            //  ,
                            //  &ap->rssi, &ap->bssid[0], &ap->bssid[1], &ap->bssid[2], &ap->bssid[3], &ap->bssid[4],
                            //  &ap->bssid[5], &channel
                            );  //  "&channel" was previously "&ap->channel", which is incorrect because "%d" assigns an int not uint8_t.
    ap->channel = (uint8_t) channel;
    ap->security = sec < 5 ? (nsapi_security_t)sec : NSAPI_SECURITY_UNKNOWN;
    console_printf(ret ? "ESP ap OK\n" : "ESP ap FAILED\n"); console_flush();
    return ret;
}

#ifdef NOTUSED  //  Test for vsscanf() bug in Baselibc...
    int sec, rssi;
    int rc = sscanf(
        "+CWLAP:(3,\"HP-Print-54-Officejet 0000\",-74,"
        //  "\"8c:dc:d4:00:00:00\",1,-34,0)"
        ,
        "+CWLAP:(%d,\"%32[^\"]\",%d,"
        //  "\"%hhx:%hhx:%hhx:%hhx:%hhx:%hhx\",%d,"
        , 
        &sec, ap->ssid, &rssi 
        //  ,&ap->bssid[0], &ap->bssid[1], &ap->bssid[2], &ap->bssid[3], &ap->bssid[4], &ap->bssid[5], &channel
    );
    //  Should return rc=3, rssi=-74.  Buggy version of vsscanf() return rc=2, rssi=some other value.
    console_printf("sscanf %d\n", rc);
#endif  //  NOTUSED
