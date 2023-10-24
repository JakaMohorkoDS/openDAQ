//------------------------------------------------------------------------------
// <auto-generated>
//     This code was generated by a tool.
//
//     Changes to this file may cause incorrect behavior and will be lost if
//     the code is regenerated.
//
//     RTGen (PythonGenerator).
// </auto-generated>
//------------------------------------------------------------------------------

/*
 * Copyright 2022-2023 Blueberry d.o.o.
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

#include "py_opendaq/py_opendaq.h"
#include "py_core_types/py_converter.h"

PyDaqIntf<daq::IPacket, daq::IBaseObject> declareIPacket(pybind11::module_ m)
{
    py::enum_<daq::PacketType>(m, "PacketType")
        .value("None", daq::PacketType::None)
        .value("Data", daq::PacketType::Data)
        .value("Event", daq::PacketType::Event);

    return wrapInterface<daq::IPacket, daq::IBaseObject>(m, "IPacket");
}

void defineIPacket(pybind11::module_ m, PyDaqIntf<daq::IPacket, daq::IBaseObject> cls)
{
    cls.doc() = "Base packet type. Data, Value, and Event packets are all also packets. Provides the packet's unique ID that is unique to a given device, as well as the packet type.";

    cls.def_property_readonly("type",
        [](daq::IPacket *object)
        {
            const auto objectPtr = daq::PacketPtr::Borrow(object);
            return objectPtr.getType();
        },
        "Gets the packet's type.");
    cls.def("subscribe_for_destruct_notification",
        [](daq::IPacket *object, daq::IPacketDestructCallback* packetDestructCallback)
        {
            const auto objectPtr = daq::PacketPtr::Borrow(object);
            objectPtr.subscribeForDestructNotification(packetDestructCallback);
        },
        py::arg("packet_destruct_callback"),
        "Subscribes for notification when the packet is destroyed.");
    cls.def_property_readonly("ref_count",
        [](daq::IPacket *object)
        {
            const auto objectPtr = daq::PacketPtr::Borrow(object);
            return objectPtr.getRefCount();
        },
        "Gets the reference count of the packet.");
}
