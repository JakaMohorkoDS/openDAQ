#include <coretypes/validation.h>
#include <opendaq/multi_reader_impl.h>
#include <opendaq/custom_log.h>
#include <opendaq/input_port_factory.h>
#include <opendaq/reader_errors.h>
#include <opendaq/reader_utils.h>
#include <coreobjects/property_object_factory.h>
#include <coreobjects/ownable_ptr.h>

#include <fmt/ostream.h>
#include <thread>

using namespace std::chrono;

using Milliseconds = duration<double, std::milli>;

template <>
struct fmt::formatter<daq::Comparable> : ostream_formatter
{
};

BEGIN_NAMESPACE_OPENDAQ


MultiReaderImpl::MultiReaderImpl(const ListPtr<IComponent>& list,
                                 SampleType valueReadType,
                                 SampleType domainReadType,
                                 ReadMode mode,
                                 ReadTimeoutType timeoutType,
                                 std::int64_t requiredCommonSampleRate,
                                 Bool startOnFullUnitOfDomain)
    : requiredCommonSampleRate(requiredCommonSampleRate)
    , startOnFullUnitOfDomain(startOnFullUnitOfDomain)
{
    this->internalAddRef();
    try
    {
        auto ports = CheckPreconditions(list);
        loggerComponent = ports[0].getContext().getLogger().getOrAddComponent("MultiReader");

        portBinder = PropertyObject();
        connectPorts(ports, valueReadType, domainReadType, mode);

        updateCommonSampleRateAndDividers();

        SizeT min{};
        SyncStatus syncStatus{};
        ErrCode errCode = synchronize(min, syncStatus);

        checkErrorInfo(errCode);
    }
    catch (...)
    {
        this->releaseWeakRefOnException();
        throw;
    }

}

MultiReaderImpl::MultiReaderImpl(MultiReaderImpl* old,
                                 SampleType valueReadType,
                                 SampleType domainReadType)
    : loggerComponent(old->loggerComponent)
{
    std::scoped_lock lock(old->mutex);
    old->invalid = true;
    portBinder = old->portBinder;
    startOnFullUnitOfDomain = old->startOnFullUnitOfDomain;

    CheckPreconditions(old->getSignals(), false);
    commonSampleRate = old->commonSampleRate;
    requiredCommonSampleRate = old->requiredCommonSampleRate;

    this->internalAddRef();
    auto listener = this->thisPtr<InputPortNotificationsPtr>();

    for (auto& signal : old->signals)
    {
        signals.emplace_back(signal, listener, valueReadType, domainReadType);
    }

    updateCommonSampleRateAndDividers();
}

MultiReaderImpl::MultiReaderImpl(const ReaderConfigPtr& readerConfig,
                                 SampleType valueReadType,
                                 SampleType domainReadType,
                                 ReadMode mode)
{
    if (!readerConfig.assigned())
        throw ArgumentNullException("Existing reader must not be null");

    readerConfig.markAsInvalid();

    SignalInfo sigInfo {
        nullptr,
        nullptr,
        readerConfig.getValueTransformFunction(),
        readerConfig.getDomainTransformFunction(),
        mode,
        loggerComponent
    };

    this->internalAddRef();
    auto listener = this->thisPtr<InputPortNotificationsPtr>();

    auto ports = readerConfig.getInputPorts();

    CheckPreconditions(ports, false);
  
    for (const auto& port : ports)
    {
        sigInfo.port = port;
        signals.emplace_back(sigInfo, listener, valueReadType, domainReadType);
    }

    updateCommonSampleRateAndDividers();
}

MultiReaderImpl::MultiReaderImpl(const MultiReaderBuilderPtr& builder)
    : requiredCommonSampleRate(builder.getRequiredCommonSampleRate())
    , startOnFullUnitOfDomain(builder.getStartOnFullUnitOfDomain())
{
    auto ports = CheckPreconditions(builder.getSourceComponents(), false);
    loggerComponent = ports[0].getContext().getLogger().getOrAddComponent("MultiReader");

    this->internalAddRef();

    portBinder = PropertyObject();
    connectPorts(ports, builder.getValueReadType(), builder.getDomainReadType(), builder.getReadMode());

    updateCommonSampleRateAndDividers();

    SizeT min{};
    SyncStatus syncStatus{};
    ErrCode errCode = synchronize(min, syncStatus);

    checkErrorInfo(errCode);
}

MultiReaderImpl::~MultiReaderImpl()
{
    if (!portBinder.assigned())
        for (const auto& signal : signals)
            signal.port.remove();
}

ListPtr<ISignal> MultiReaderImpl::getSignals() const
{
    auto list = List<ISignal>();
    for (auto& signal : signals)
    {
        list.pushBack(signal.connection.getSignal());
    }
    return list;
}

static void checkSameDomain(const ListPtr<IInputPortConfig>& list)
{
    StringPtr domainUnit;
    StringPtr domainQuantity;

    for (const auto& port : list)
    {
        auto signal = port.getSignal();
        auto domain = signal.getDomainSignal();
        if (!domain.assigned())
        {
            throw InvalidParameterException(R"(Signal "{}" does not have a domain signal set.)", signal.getLocalId());
        }

        auto unit = domain.getDescriptor().getUnit();
        if (!unit.assigned())
        {
            throw InvalidParameterException(R"(Signal "{}" does not have a domain unit set.)", signal.getLocalId());
        }

        if (!domainQuantity.assigned() || domainQuantity.getLength() == 0)
        {
            domainQuantity = unit.getQuantity();
            domainUnit = unit.getSymbol();

            if (!domainQuantity.assigned() || domainQuantity.getLength() == 0)
            {
                throw InvalidParameterException(R"(Signal "{}" does not have a domain quantity set.)", signal.getLocalId());
            }

            if (domainQuantity != "time")
            {
                throw NotSupportedException(
                    R"(Signal "{}" domain quantity is not "time" but "{}" which is not currently supported.)",
                    signal.getLocalId(),
                    domainQuantity
                );
            }

            if (domainUnit != "s")
            {
                throw NotSupportedException(
                    R"(Signal "{}" domain unit is not "s" but "{}" which is not currently supported.)",
                    signal.getLocalId(),
                    domainUnit
                );
            }
        }
        else
        {
            if (domainQuantity != unit.getQuantity())
            {
                throw InvalidStateException(R"(Signal "{}" domain quantity does not match with others.)", signal.getLocalId());
            }

            if (domainUnit != unit.getSymbol())
            {
                throw InvalidStateException(R"(Signal "{}" domain unit does not match with others.)", signal.getLocalId());
            }
        }
    }
}

void MultiReaderImpl::updateCommonSampleRateAndDividers()
{
    if (requiredCommonSampleRate > 0)
    {
        commonSampleRate = requiredCommonSampleRate;
    }
    else
    {
        commonSampleRate = 1;
        for (const auto& signal : signals)
        {
            commonSampleRate = std::lcm<std::int64_t>(signal.sampleRate, commonSampleRate);
        }
    }

    for (auto& signal : signals)
    {
        signal.setCommonSampleRate(commonSampleRate);
        if (signal.invalid)
        {
            signal.setCommonSampleRate(commonSampleRate);
            throw InvalidParameterException("Signal sample rate does not match required common sample rate");
        }
    }

    sampleRateDividerLcm = 1;

    for (const auto& signal : signals)
    {
        sampleRateDividerLcm = std::lcm(signal.sampleRateDivider, sampleRateDividerLcm);
    }
}

template <typename T>
bool MultiReaderImpl::ListElementsHaveSameType(const ListPtr<IBaseObject>& list)
{
    for (const auto & el : list)
        if (el.asPtrOrNull<T>() == nullptr)
            return false;
    return true;
}

ListPtr<IInputPortConfig> MultiReaderImpl::CheckPreconditions(const ListPtr<IComponent>& list, bool overrideMethod)
{
    if (!list.assigned())
        throw NotAssignedException("List of inputs is not assigned");
    if (list.getCount() == 0)
        throw InvalidParameterException("Need at least one signal.");

    auto portList = List<IInputPortConfig>();   
    for (const auto & el : list)
    {
        if (auto signal = el.asPtrOrNull<ISignal>(); signal.assigned())
        {
            auto port = InputPort(signal.getContext(), nullptr, fmt::format("Read signal {}", signal.getLocalId()));
            if (overrideMethod)
                port.setNotificationMethod(PacketReadyNotification::SameThread);
            port.connect(signal);
            portList.pushBack(port);
        }
        else if (auto port = el.asPtrOrNull<IInputPortConfig>(); port.assigned())
        {
            if (overrideMethod)
                port.setNotificationMethod(PacketReadyNotification::Scheduler);
            portList.pushBack(port);
        }
        else
        {
            throw InvalidParameterException("One of the elements of input list is not signal or input port");
        }
    }

    checkSameDomain(portList);
    return portList;
}

void MultiReaderImpl::setStartInfo()
{
    LOG_T("<----")
    LOG_T("Setting start info:")

    RatioPtr maxResolution = signals[0].domainInfo.resolution;
    system_clock::time_point minEpoch = signals[0].domainInfo.epoch;
    for (auto& sigInfo : signals)
    {
        if (sigInfo.domainInfo.epoch < minEpoch)
        {
            minEpoch = sigInfo.domainInfo.epoch;
        }

        if (static_cast<double>(sigInfo.domainInfo.resolution) < static_cast<double>(maxResolution))
        {
            maxResolution = sigInfo.domainInfo.resolution;
        }
    }

    auto systemClockResolution = system_clock::period::num / static_cast<double>(system_clock::period::den);
    if (systemClockResolution < static_cast<double>(maxResolution))
    {
        maxResolution = Ratio(system_clock::period::num, system_clock::period::den);
    }

    readResolution = maxResolution;
    readOrigin = date::format("%FT%TZ", minEpoch);

    LOG_T("MaxResolution: {}", maxResolution)
    LOG_T("MinEpoch: {}", minEpoch)

    for (auto& sigInfo : signals)
    {
        sigInfo.setStartInfo(minEpoch, maxResolution);
    }
}

void MultiReaderImpl::connectPorts(const ListPtr<IInputPortConfig>& inputPorts,
                                     SampleType valueRead,
                                     SampleType domainRead,
                                     ReadMode mode)
{
    auto listener = this->thisPtr<InputPortNotificationsPtr>();

    for (const auto& port : inputPorts)
    {
        port.asPtr<IOwnable>().setOwner(portBinder);
        port.setListener(listener);

        auto& sigInfo = signals.emplace_back(port, valueRead, domainRead, mode, loggerComponent);
        if (sigInfo.connection.assigned())
            sigInfo.handleDescriptorChanged(sigInfo.connection.dequeue());
    }
}

ErrCode MultiReaderImpl::setOnDescriptorChanged(IFunction* callback)
{
    std::scoped_lock lock(mutex);

    for (auto& signal : signals)
    {
        signal.changeCallback = callback;
    }
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::setOnDataAvailable(IProcedure* callback)
{
    std::scoped_lock lock(mutex);

    readCallback = callback;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getValueReadType(SampleType* sampleType)
{
    OPENDAQ_PARAM_NOT_NULL(sampleType);

    *sampleType = signals[0].valueReader->getReadType();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getDomainReadType(SampleType* sampleType)
{
    OPENDAQ_PARAM_NOT_NULL(sampleType);

    *sampleType = signals[0].domainReader->getReadType();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::setValueTransformFunction(IFunction* transform)
{
    std::scoped_lock lock(mutex);

    for (auto& signal : signals)
    {
        signal.valueReader->setTransformFunction(transform);
    }

    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::setDomainTransformFunction(IFunction* transform)
{
    std::scoped_lock lock(mutex);

    for (auto& signal : signals)
    {
        signal.domainReader->setTransformFunction(transform);
    }

    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getReadMode(ReadMode* mode)
{
    OPENDAQ_PARAM_NOT_NULL(mode);

    *mode = signals[0].readMode;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getAvailableCount(SizeT* count)
{
    OPENDAQ_PARAM_NOT_NULL(count);

    std::lock_guard lock(mutex);

    ErrCode errCode = readUntilFirstDataPacket();
    if (OPENDAQ_FAILED(errCode))
    {
        if (errCode == OPENDAQ_ERR_INVALID_DATA)
        {
            *count = 0;
            invalid = true;

            clearErrorInfo();
            return OPENDAQ_SUCCESS;
        }
        return errCode;
    }

    SizeT min{};
    SyncStatus syncStatus{};
    errCode = synchronize(min, syncStatus);
    if (OPENDAQ_FAILED(errCode))
    {
        return errCode;
    }

    *count = syncStatus == SyncStatus::Synchronized
        ? min
        : 0;

    *count = (*count / sampleRateDividerLcm) * sampleRateDividerLcm;

    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::read(void* samples, SizeT* count, SizeT timeoutMs, IReaderStatus** status)
{
    OPENDAQ_PARAM_NOT_NULL(samples);
    OPENDAQ_PARAM_NOT_NULL(count);

    std::scoped_lock lock(mutex);

    if (invalid)
        return makeErrorInfo(OPENDAQ_ERR_INVALID_DATA, errorMessage, nullptr);

    SizeT samplesToRead = (*count / sampleRateDividerLcm) * sampleRateDividerLcm;
    prepare((void**)samples, samplesToRead, milliseconds(timeoutMs));

    ErrCode errCode = readPackets();

    SizeT samplesRead = samplesToRead - remainingSamplesToRead;
    *count = samplesRead;
    return errCode;
}

ErrCode MultiReaderImpl::readWithDomain(void* samples, void* domain, SizeT* count, SizeT timeoutMs, IReaderStatus** status)
{
    OPENDAQ_PARAM_NOT_NULL(samples);
    OPENDAQ_PARAM_NOT_NULL(domain);
    OPENDAQ_PARAM_NOT_NULL(count);

    std::scoped_lock lock(mutex);

    if (invalid)
        return makeErrorInfo(OPENDAQ_ERR_INVALID_DATA, errorMessage, nullptr);

    SizeT samplesToRead = (*count / sampleRateDividerLcm) * sampleRateDividerLcm;
    prepareWithDomain((void**)samples, (void**)domain, samplesToRead, milliseconds(timeoutMs));

    ErrCode errCode = readPackets();

    SizeT samplesRead = samplesToRead - remainingSamplesToRead;
    *count = samplesRead;
    return errCode;
}

ErrCode MultiReaderImpl::skipSamples(SizeT* count)
{
    OPENDAQ_PARAM_NOT_NULL(count);

    std::scoped_lock lock(mutex);

    if (invalid)
        return makeErrorInfo(OPENDAQ_ERR_INVALID_DATA, errorMessage, nullptr);

    const SizeT samplesToRead = *count;
    prepare(nullptr, samplesToRead, milliseconds(0));

    const ErrCode errCode = readPackets();

    const SizeT samplesRead = samplesToRead - remainingSamplesToRead;
    *count = samplesRead;
    return errCode;
}

SizeT MultiReaderImpl::getMinSamplesAvailable(bool acrossDescriptorChanges) const
{
    SizeT min = std::numeric_limits<SizeT>::max();
    for (const auto& signal : signals)
    {
        auto sigSamples = signal.getAvailable(acrossDescriptorChanges);
        if (sigSamples < min)
        {
            min = sigSamples;
        }
    }

    return min;
}

SyncStatus MultiReaderImpl::getSyncStatus() const
{
    SyncStatus status = SyncStatus::Unsynchronized;
    for (const auto& signal : signals)
    {
        switch (signal.synced)
        {
            case SyncStatus::Unsynchronized:
                return signal.synced;
            case SyncStatus::Synchronizing:
                status = signal.synced;
                break;
            case SyncStatus::Synchronized:
                if (status == SyncStatus::Unsynchronized)
                    status = signal.synced;
                break;
        }
    }
    return status;
}

ErrCode MultiReaderImpl::readUntilFirstDataPacket()
{
    try
    {
        for (auto& signal : signals)
        {
            signal.readUntilNextDataPacket();
        }
    }
    catch (const DaqException& e)
    {
        return errorFromException(e, nullptr);
    }
    catch (const std::exception& e)
    {
        return errorFromException(e);
    }
    catch (...)
    {
        return OPENDAQ_ERR_GENERALERROR;
    }

    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::synchronize(SizeT& min, SyncStatus& syncStatus)
{
    min = getMinSamplesAvailable();
    syncStatus = getSyncStatus();

    if (syncStatus != SyncStatus::Synchronized)
    {
        try
        {
            if (min > 1 && syncStatus != SyncStatus::Synchronizing)
            {
                setStartInfo();
                readDomainStart();
                sync();
            }
            else if (syncStatus == SyncStatus::Synchronizing)
            {
                sync();
            }

            // Re-check samples available after dropping them to sync
            syncStatus = getSyncStatus();
            if (syncStatus == SyncStatus::Synchronized)
            {
                min = getMinSamplesAvailable();
            }
        }
        catch (const DaqException& e)
        {
            return errorFromException(e, nullptr);
        }
        catch (const std::exception& e)
        {
            return errorFromException(e);
        }
        catch (...)
        {
            return OPENDAQ_ERR_GENERALERROR;
        }
    }
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::readPackets()
{
    ErrCode errCode = OPENDAQ_SUCCESS;

    [[maybe_unused]]
    auto count = remainingSamplesToRead;

    ReadInfo::Duration remainingTime = timeout.count() == 0
                                           ? 1ms
                                           : timeout;

    while (remainingSamplesToRead > 0 && remainingTime >= 1ms)
    {
        errCode = readUntilFirstDataPacket();
        if (OPENDAQ_FAILED(errCode))
        {
            if (errCode == OPENDAQ_ERR_INVALID_DATA)
            {
                invalid = true;
                errorMessage = reader::getErrorMessage();

                clearErrorInfo();
                return OPENDAQ_SUCCESS;
            }
            return errCode;
        }

        if (timeout.count() != 0)
        {
            remainingTime = timeout - durationFromStart();
            if (remainingTime < milliseconds(1))
            {
                LOGP_T("Time exceeded when reading non-data packets")
                break;
            }
        }

        SizeT min{};
        SyncStatus syncStatus{};

        errCode = synchronize(min, syncStatus);
        if (OPENDAQ_FAILED(errCode))
        {
            return errCode;
        }

        if (syncStatus == SyncStatus::Synchronized && min > 0u)
        {
            auto toRead = std::min(min, remainingSamplesToRead);

#if (OPENDAQ_LOG_LEVEL <= OPENDAQ_LOG_LEVEL_TRACE)
            auto start = std::chrono::steady_clock::now();
#endif
            readSamples(toRead);

#if (OPENDAQ_LOG_LEVEL <= OPENDAQ_LOG_LEVEL_TRACE)
            auto end = std::chrono::steady_clock::now();
            LOG_T("Read {} / {} [{} left] took {} ms with {} ms remaining",
                  toRead,
                  count,
                  remainingSamplesToRead,
                  std::chrono::duration_cast<Milliseconds>(end - start).count(),
                  std::chrono::duration_cast<Milliseconds>(timeout - durationFromStart()).count()
            )
#endif
        }

        if (timeout.count() != 0)
        {
            auto fromStart = durationFromStart();
            remainingTime = timeout - fromStart;

            LOG_T("Time spent: {} ms | Remaining time: {} ms",
                duration_cast<milliseconds>(fromStart).count(),
                duration_cast<milliseconds>(remainingTime).count()
            );

            std::this_thread::sleep_for(1ms);
        }
        else if (min == 0)
        {
            break;
        }
    }

    {
        auto fromStart = durationFromStart();

         LOG_T("FINAL> Time spent: {} ms | Remaining time: {} ms",
             duration_cast<milliseconds>(fromStart).count(),
             duration_cast<milliseconds>(remainingTime).count()
         )
    }

    return errCode;
}

// Listener

ErrCode MultiReaderImpl::acceptsSignal(IInputPort* port, ISignal* signal, Bool* accept)
{
    OPENDAQ_PARAM_NOT_NULL(port);
    OPENDAQ_PARAM_NOT_NULL(signal);
    OPENDAQ_PARAM_NOT_NULL(accept);

    *accept = true;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::connected(IInputPort* port)
{
    OPENDAQ_PARAM_NOT_NULL(port);

    auto findSigByPort = [port](const SignalReader& signal) {
            return signal.port == port;
        };

    std::scoped_lock lock(mutex);
    if (signals.empty())
        return OPENDAQ_SUCCESS;

    ErrCode errCode = OPENDAQ_SUCCESS;
    auto sigInfo = std::find_if(signals.begin(), signals.end(), findSigByPort);

    if (sigInfo != signals.end())
    {
        sigInfo->connection = sigInfo->port.getConnection();
        sigInfo->handleDescriptorChanged(sigInfo->connection.dequeue());

        // check new signals
        auto portList = List<IInputPortConfig>();
        for (const auto & signalReader : signals) {
            const auto & inputPort = signalReader.port;
            if (inputPort.getSignal().assigned())
                portList.pushBack(inputPort);
        }

        checkSameDomain(portList);

        updateCommonSampleRateAndDividers();

        SizeT min{};
        SyncStatus syncStatus{};
        errCode = synchronize(min, syncStatus);

        checkErrorInfo(errCode);
    }
    return errCode;
}

ErrCode MultiReaderImpl::disconnected(IInputPort* port)
{
    OPENDAQ_PARAM_NOT_NULL(port);
    auto findSigByPort = [port](const SignalReader& signal) {
            return signal.port == port;
        };

    std::scoped_lock lock(mutex);
    if (signals.empty())
        return OPENDAQ_SUCCESS;

    auto sigInfo = std::find_if(signals.begin(), signals.end(), findSigByPort);

    if (sigInfo != signals.end())
        sigInfo->connection = nullptr;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::packetReceived(IInputPort* inputPort)
{
    ProcedurePtr callback;

    {
        std::scoped_lock lock(mutex);
        callback = readCallback;
        if (!callback.assigned())
            return OPENDAQ_SUCCESS;
    }

    SizeT count;
    getAvailableCount(&count);
    if (count)
        return wrapHandler(callback);

    return OPENDAQ_SUCCESS;
}

#pragma region MultiReaderInfo

void MultiReaderImpl::prepare(void** outValues, SizeT count, std::chrono::milliseconds timeoutTime)
{
    remainingSamplesToRead = count;
    values = outValues;

    domainValues = nullptr;

    timeout = std::chrono::duration_cast<Duration>(timeoutTime);
    startTime = std::chrono::steady_clock::now();

    const SizeT alignedCount = (count / sampleRateDividerLcm) * sampleRateDividerLcm;

    const auto signalsNum = signals.size();
    for (SizeT i = 0u; i < signalsNum; ++i)
    {
        const auto outPtr = outValues != nullptr ? outValues[i] : nullptr;
        signals[i].prepare(outPtr, alignedCount, timeoutTime);
    }
}

void MultiReaderImpl::prepareWithDomain(void** outValues, void** domain, SizeT count, std::chrono::milliseconds timeoutTime)
{
    remainingSamplesToRead = count;
    values = outValues;

    domainValues = domain;

    timeout = std::chrono::duration_cast<Duration>(timeoutTime);
    startTime = std::chrono::steady_clock::now();

    const SizeT alignedCount = (count / sampleRateDividerLcm) * sampleRateDividerLcm;

    auto signalsNum = signals.size();
    for (SizeT i = 0u; i < signalsNum; ++i)
    {
        signals[i].prepareWithDomain(outValues[i], domain[i], alignedCount, timeoutTime);
    }
}

MultiReaderImpl::Duration MultiReaderImpl::durationFromStart() const
{
    return std::chrono::duration_cast<Duration>(Clock::now() - startTime);
}

void MultiReaderImpl::readSamples(SizeT samples)
{
    auto signalsNum = signals.size();
    for (SizeT i = 0u; i < signalsNum; ++i)
    {
        signals[i].info.remainingToRead = samples / signals[i].sampleRateDivider;
        signals[i].readPackets();
    }

    remainingSamplesToRead -= samples;
}

void MultiReaderImpl::readDomainStart()
{
    assert(getSyncStatus() != SyncStatus::Synchronized);

    LOG_T("---\n");

    for (auto& signal : signals)
    {
        auto sigStart = signal.readStartDomain();
        if (!commonStart)
        {
            commonStart = std::move(sigStart);
        }
        else
        {
            if (*commonStart < *sigStart)
            {
                commonStart = std::move(sigStart);
            }
        }
    }

    LOG_T("---");
    LOG_T("DomainStart: {}", *commonStart);

    if (startOnFullUnitOfDomain)
    {
        commonStart->roundUpOnUnitOfDomain();
        LOG_T("Rounded DomainStart: {}", *commonStart);
    }
    else
    {
        const RatioPtr interval = Ratio(sampleRateDividerLcm, commonSampleRate).simplify();
        commonStart->roundUpOnDomainInterval(interval);
        LOG_T("Aligned DomainStart: {}", *commonStart);
    }
}

void MultiReaderImpl::sync()
{
    bool synced = true;
    for (auto& signal : signals)
    {
        synced = signal.sync(*commonStart) && synced;
    }

    LOG_T("Synced: {}", synced);
}

#pragma endregion MultiReaderInfo

ErrCode MultiReaderImpl::getTickResolution(IRatio** resolution)
{
    if (resolution == nullptr)
    {
        return OPENDAQ_ERR_ARGUMENT_NULL;
    }

    *resolution = readResolution.addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getOrigin(IString** origin)
{
    if (origin == nullptr)
    {
        return OPENDAQ_ERR_ARGUMENT_NULL;
    }

    *origin = readOrigin.addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getOffset(void* domainStart)
{
    if (domainStart == nullptr)
    {
        return OPENDAQ_ERR_ARGUMENT_NULL;
    }

    if (commonStart)
    {
        commonStart->getValue(domainStart);
        return OPENDAQ_SUCCESS;
    }

    return OPENDAQ_IGNORED;
}

ErrCode INTERFACE_FUNC MultiReaderImpl::getCommonSampleRate(Int* commonSampleRate)
{
    if (commonSampleRate == nullptr)
    {
        return OPENDAQ_ERR_ARGUMENT_NULL;
    }

    *commonSampleRate = this->commonSampleRate;

    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getIsSynchronized(Bool* isSynchronized)
{
    OPENDAQ_PARAM_NOT_NULL(isSynchronized);

    *isSynchronized = static_cast<bool>(commonStart);

    return OPENDAQ_SUCCESS;
}

#pragma region ReaderConfig

ErrCode MultiReaderImpl::getOnDescriptorChanged(IFunction** callback)
{
    OPENDAQ_PARAM_NOT_NULL(callback);
    std::scoped_lock lock(mutex);

    *callback = signals[0].changeCallback;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getValueTransformFunction(IFunction** transform)
{
    OPENDAQ_PARAM_NOT_NULL(transform);
    std::scoped_lock lock(mutex);

    *transform = signals[0].valueReader->getTransformFunction().addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getDomainTransformFunction(IFunction** transform)
{
    OPENDAQ_PARAM_NOT_NULL(transform);
    std::scoped_lock lock(mutex);

    *transform = signals[0].domainReader->getTransformFunction().addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getInputPorts(IList** ports)
{
    OPENDAQ_PARAM_NOT_NULL(ports);

    auto list = List<IInputPortConfig>();
    for (auto& signal : signals)
    {
        list.pushBack(signal.port);
    }

    *ports = list.detach();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getReadTimeoutType(ReadTimeoutType* timeoutType)
{
    OPENDAQ_PARAM_NOT_NULL(timeoutType);

    *timeoutType = ReadTimeoutType::All;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::markAsInvalid()
{
    std::unique_lock lock(mutex);
    invalid = true;

    return OPENDAQ_SUCCESS;
}

#pragma endregion ReaderConfig

OPENDAQ_DEFINE_CLASS_FACTORY(
    LIBRARY_FACTORY, MultiReader,
    IList*, signals,
    SampleType, valueReadType,
    SampleType, domainReadType,
    ReadMode, mode,
    ReadTimeoutType, timeoutType
)

OPENDAQ_DEFINE_CLASS_FACTORY_WITH_INTERFACE_AND_CREATEFUNC_OBJ(
    LIBRARY_FACTORY, MultiReaderImpl, IMultiReader, createMultiReaderEx,
    IList*, signals,
    SampleType, valueReadType,
    SampleType, domainReadType,
    ReadMode, mode,
    ReadTimeoutType, timeoutType,
    Int, requiredCommonSampleRate,
    Bool, startOnFullUnitOfDomain
)


template <>
struct ObjectCreator<IMultiReader>
{
    static ErrCode Create(IMultiReader** out,
                          IMultiReader* toCopy,
                          SampleType valueReadType,
                          SampleType domainReadType
                         ) noexcept
    {
        OPENDAQ_PARAM_NOT_NULL(out);

        if (toCopy == nullptr)
        {
            return makeErrorInfo(OPENDAQ_ERR_ARGUMENT_NULL, "Existing reader must not be null", nullptr);
        }

        ReadMode mode;
        toCopy->getReadMode(&mode);

        auto old = ReaderConfigPtr::Borrow(toCopy);
        auto impl = dynamic_cast<MultiReaderImpl*>(old.getObject());

        return impl != nullptr
            ? createObject<IMultiReader, MultiReaderImpl>(out, impl, valueReadType, domainReadType)
            : createObject<IMultiReader, MultiReaderImpl>(out, old, valueReadType, domainReadType, mode);
    }
};

OPENDAQ_DEFINE_CUSTOM_CLASS_FACTORY_WITH_INTERFACE_AND_CREATEFUNC_OBJ(
    LIBRARY_FACTORY, IMultiReader, createMultiReaderFromExisting,
    IMultiReader*, invalidatedReader,
    SampleType, valueReadType,
    SampleType, domainReadType
)

extern "C"
daq::ErrCode PUBLIC_EXPORT createMultiReaderFromBuilder(IMultiReader** objTmp, IMultiReaderBuilder* builder)
{
    return daq::createObject<IMultiReader, MultiReaderImpl>(objTmp, builder);
}

END_NAMESPACE_OPENDAQ
