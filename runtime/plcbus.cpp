#include "plcbus.h"
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <thread>
#include <mutex>
#include <cstring>
#include <iostream>
#include <fstream>
#include "plcstate.h"
#include <arpa/inet.h>
#include <plcdata.h>

#ifdef FPGA_ALLOW
#include "hps_0_arm_a9_0.h"
#define RE_SET (*((char*)LEDS_BASE) |= (1 << 7))
#define RE_CLR (*((char*)LEDS_BASE) &= ~(1 << 7))
#else
#define RE_SET
#define RE_CLR
#endif

std::mutex mtx_IO;

bool PLCBus::init()
{
    if (!init_UART())
        return false;
    if (!search_modules())
        return false;
    return true;
}

void PLCBus::copy_inputs()
{
    mtx_IO.lock();
    plc_inputs.update_inputs(m_PIP);
    mtx_IO.unlock();
}

void PLCBus::copy_outputs()
{
    mtx_IO.lock();
    plc_outputs.update_outputs(m_POP);
    mtx_IO.unlock();
}

void PLCBus::bus_proc()
{
    mtx_IO.lock();

    // read inputs
    for (uint32_t i=0 ; i<m_count ; ++i)
    {
        ModuleInfo* module = &m_modules_list[i];
        if ((module->input_size == 0) ||
            (module->rack != 0))    // another racks - only from communications!
            continue;

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        RE_SET;

        m_send.from = 0;
        m_send.to = module->rack_idx;
        m_send.request = EBusRequest::READ_INPUTS;
        m_send.data_size = 0;

        write(m_bus_dev, &m_send, sizeof(BusMessage));

        m_recv.request = EBusRequest::UNKNOWN;
        m_recv.reply = EBusReply::UNKNOWN;

        RE_CLR;
        //std::this_thread::sleep_for(std::chrono::milliseconds(BUS_WAIT_TIME_MS));
        read(m_bus_dev, &m_recv, sizeof(BusMessage));

        if (m_recv.request == EBusRequest::UNKNOWN)
        {
            PLCState::to_error();
            printf("Unable to read inputs from module #%i\n", module->rack_idx);
            break;
        }

        switch (m_recv.reply)
        {
        case EBusReply::UNKNOWN:
            PLCState::to_error();
            printf("Invlid reply (read inputs) from from module #%i\n", module->rack_idx);
            return;
        case EBusReply::OK:
            std::memcpy(&m_PIP[module->input_start], &m_recv.data, module->input_size);
            printf("Read inputs from module (index:%i):\n",
                   module->rack_idx);

            // check error on module
            if (m_recv.module_info.state.fault)
                PLCState::to_error();
            break;
        case EBusReply::FAIL:
            return;
        }
    }

    // write outputs
    for (uint32_t i=0 ; i<m_count ; ++i)
    {
        ModuleInfo* module = &m_modules_list[i];
        if ((module->output_size == 0) ||
            (module->rack != 0))    // another racks - only from communications!
            continue;

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        RE_SET;

        m_send.from = 0;
        m_send.to = module->rack_idx;
        m_send.request = EBusRequest::WRITE_OUTPUTS;
        m_send.data_size = module->output_size;
        std::memcpy(&m_send.data, &m_POP[module->output_start], module->output_size);

        write(m_bus_dev, &m_send, sizeof(BusMessage));

        m_recv.request = EBusRequest::UNKNOWN;
        m_recv.reply = EBusReply::UNKNOWN;

        RE_CLR;
        //std::this_thread::sleep_for(std::chrono::milliseconds(BUS_WAIT_TIME_MS));
        read(m_bus_dev, &m_recv, sizeof(BusMessage));

        if (m_recv.request == EBusRequest::UNKNOWN)
        {
            PLCState::to_error();
            printf("Unable to write outputs to module #%i\n", module->rack_idx);
            break;
        }

        switch (m_recv.reply)
        {
        case EBusReply::UNKNOWN:
            PLCState::to_error();
            printf("Invlid reply (write outputs) from from module #%i\n", module->rack_idx);
            return;
        case EBusReply::OK:
            // check error on module
            if (m_recv.module_info.state.fault)
                PLCState::to_error();
            break;
        case EBusReply::FAIL:
            return;
        }
    }

    mtx_IO.unlock();
}

bool PLCBus::load_config()
{
    std::ifstream str(RT_ROOT_PATH "hw.json", std::ifstream::binary);
    if (!str)
    {
        return false;
    }
    str.seekg (0, str.end);
    int length = str.tellg();
    str.seekg (0, str.beg);
    char* buf = new char[length];
    str.read(buf, length);
    str.close();

    Json::Value root;
    Json::CharReaderBuilder b;
    b.settings_["allowSingleQuotes"] = true;
    Json::CharReader* reader(b.newCharReader());
    JSONCPP_STRING errs;
    if (!reader->parse(buf, buf + length, &root, &errs))
    {
        std::cout << errs << std::endl;
        return false;
    }

    delete reader;
    delete[] buf;

    //TODO: load hardware
    Json::Value &modules = root["modules"];
    m_count = modules.size();
    for (uint32_t i=0 ; i<m_count ; ++i)
    {
        Json::Value &module = modules[i];
        if (!load_module_info(m_modules_list[i], module))
            return false;
    }

    return true;
}

bool PLCBus::init_UART()
{
    m_bus_dev = open(BUS_UART_DEVICE, O_RDWR | O_NOCTTY | O_NDELAY);
    if (m_bus_dev == -1)
    {
        return false;
    }
    struct termios options;
    tcgetattr(m_bus_dev, &options);
    options.c_cflag = BUS_UART_BRATE | CS8 | CLOCAL | CREAD;    //<Set baud rate
    options.c_iflag = IGNPAR;
    options.c_oflag = 0;
    options.c_lflag = 0;
    tcflush(m_bus_dev, TCIFLUSH);
    tcsetattr(m_bus_dev, TCSANOW, &options);

    return true;
}

bool PLCBus::search_modules()
{
    m_send.from = 0;
    m_send.to = (uint32_t)-1;
    m_send.request = EBusRequest::FIND_DEVICE;
    m_send.data_size = 0;

    for (uint32_t i=0 ; i<m_count ; ++i)
    {
        ModuleInfo* module = &m_modules_list[i];
        // skip self
        if ((module->rack == 0) &&
            (module->rack_idx == 0) &&
            ((module->type & MODULE_TYPE_CPU) ==MODULE_TYPE_CPU))
        {
            module->finded = true;
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        //copy module info to send buffer
        m_send.to = module->rack_idx;
        m_send.module_info = *module;

        RE_SET;
        write(m_bus_dev, &m_send, sizeof(BusMessage));

        m_recv.request = EBusRequest::UNKNOWN;
        m_recv.reply = EBusReply::UNKNOWN;

        RE_CLR;
        //std::this_thread::sleep_for(std::chrono::milliseconds(BUS_WAIT_TIME_MS));
        read(m_bus_dev, &m_recv, sizeof(BusMessage));

        if (m_recv.request == EBusRequest::UNKNOWN)
            break;

        switch (m_recv.reply)
        {
        case EBusReply::UNKNOWN:
            PLCState::to_error();
            return true;
        case EBusReply::OK:
            module->finded = true;
            break;
        case EBusReply::FAIL:
            return false;
        }
    }
    return true;
}

bool PLCBus::load_module_info(ModuleInfo &module, Json::Value &info)
{
    module.type = info["type"].asUInt();
    module.sub_type = info["sub_type"].asUInt();

    module.rack = info["rack"].asUInt();
    module.rack_idx = info["rack_idx"].asUInt();

    module.input_start = info["istart"].asUInt();
    module.input_size = info["isize"].asUInt();
    module.output_start = info["ostart"].asUInt();
    module.output_size = info["osize"].asUInt();

    module.state.initialized = false;
    module.state.overrun = false;
    module.state.fault = false;

    //TODO: module-specific parameters
    if (module.type && MODULE_TYPE_PB)
        module.PB_addr = info["pba"].asUInt();
    if (module.type && MODULE_TYPE_PN)
        // string "192.168.1.1"
        module.PN_addr = inet_addr(info["pna"].asString().c_str());
    return true;
}
