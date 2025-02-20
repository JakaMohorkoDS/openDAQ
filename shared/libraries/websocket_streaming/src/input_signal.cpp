#include <websocket_streaming/input_signal.h>
#include <streaming_protocol/SubscribedSignal.hpp>
#include <websocket_streaming/signal_descriptor_converter.h>
#include <opendaq/packet_factory.h>
#include <opendaq/data_descriptor_factory.h>
#include <opendaq/sample_type_traits.h>

BEGIN_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING

using namespace daq;
using namespace daq::streaming_protocol;

InputSignal::InputSignal()
    : isDomainSignal(false)
{
}

PacketPtr InputSignal::createDataPacket(uint64_t packetOffset, const uint8_t* data, size_t size) const
{
    std::scoped_lock lock(descriptorsSync);
    auto sampleType = currentDataDescriptor.getSampleType();
    if (currentDataDescriptor.getPostScaling().assigned())
        sampleType = currentDataDescriptor.getPostScaling().getInputSampleType();

    const auto sampleSize = getSampleSize(sampleType);
    const auto sampleCount = size / sampleSize;

    auto domainPacket = DataPacket(currentDomainDataDescriptor, sampleCount, (Int) packetOffset);
    auto dataPacket = DataPacketWithDomain(domainPacket, currentDataDescriptor, sampleCount);
    std::memcpy(dataPacket.getRawData(), data, sampleCount*sampleSize);
    return dataPacket;
}

EventPacketPtr InputSignal::createDecriptorChangedPacket(bool valueChanged, bool domainChanged) const
{
    std::scoped_lock lock(descriptorsSync);
    const auto valueDesc = valueChanged ? currentDataDescriptor : nullptr;
    const auto domainDesc = domainChanged ? currentDomainDataDescriptor : nullptr;

    return DataDescriptorChangedEventPacket(valueDesc, domainDesc);
}

void InputSignal::setDataDescriptor(const DataDescriptorPtr& dataDescriptor)
{
    std::scoped_lock lock(descriptorsSync);
    currentDataDescriptor = dataDescriptor;
}

void InputSignal::setDomainDescriptor(const DataDescriptorPtr& domainDescriptor)
{
    std::scoped_lock lock(descriptorsSync);
    currentDomainDataDescriptor = domainDescriptor;
}

bool InputSignal::hasDescriptors() const
{
    std::scoped_lock lock(descriptorsSync);
    return currentDataDescriptor.assigned() && currentDomainDataDescriptor.assigned();
}

DataDescriptorPtr InputSignal::getSignalDescriptor() const
{
    std::scoped_lock lock(descriptorsSync);
    return currentDataDescriptor;
}

DataDescriptorPtr InputSignal::getDomainSignalDescriptor() const
{
    std::scoped_lock lock(descriptorsSync);
    return currentDomainDataDescriptor;
}

void InputSignal::setTableId(std::string id)
{
    tableId = id;
}

std::string InputSignal::getTableId() const
{
    return tableId;
}

void InputSignal::setIsDomainSignal(bool value)
{
    isDomainSignal = value;
}

bool InputSignal::getIsDomainSignal() const
{
    return isDomainSignal;
}

END_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING
