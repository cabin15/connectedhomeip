/*
 *   Copyright (c) 2021-2022 Project CHIP Authors
 *   All rights reserved.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 *
 */

#include "CHIPCommand.h"

#include <controller/CHIPDeviceControllerFactory.h>
#include <core/CHIPBuildConfig.h>
#include <credentials/attestation_verifier/FileAttestationTrustStore.h>
#include <lib/core/CHIPVendorIdentifiers.hpp>
#include <lib/support/CodeUtils.h>
#include <lib/support/ScopedBuffer.h>
#include <lib/support/TestGroupData.h>

#if CHIP_CONFIG_TRANSPORT_TRACE_ENABLED
#include "TraceHandlers.h"
#endif // CHIP_CONFIG_TRANSPORT_TRACE_ENABLED

using DeviceControllerFactory = chip::Controller::DeviceControllerFactory;

constexpr chip::FabricId kIdentityNullFabricId  = chip::kUndefinedFabricId;
constexpr chip::FabricId kIdentityAlphaFabricId = 1;
constexpr chip::FabricId kIdentityBetaFabricId  = 2;
constexpr chip::FabricId kIdentityGammaFabricId = 3;

namespace {
const chip::Credentials::AttestationTrustStore * GetTestFileAttestationTrustStore(const char * paaTrustStorePath)
{
    static chip::Credentials::FileAttestationTrustStore attestationTrustStore{ paaTrustStorePath };

    if (attestationTrustStore.IsInitialized())
    {
        return &attestationTrustStore;
    }
    else
    {
        return nullptr;
    }
}
} // namespace

CHIP_ERROR CHIPCommand::Run()
{
    StartTracing();

#if CHIP_DEVICE_LAYER_TARGET_LINUX && CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE
    // By default, Linux device is configured as a BLE peripheral while the controller needs a BLE central.
    ReturnLogErrorOnFailure(chip::DeviceLayer::Internal::BLEMgrImpl().ConfigureBle(mBleAdapterId.ValueOr(0), true));
#endif

    ReturnLogErrorOnFailure(mDefaultStorage.Init());

    chip::Controller::FactoryInitParams factoryInitParams;
    factoryInitParams.fabricIndependentStorage = &mDefaultStorage;
    uint16_t port                              = mDefaultStorage.GetListenPort();
    if (port != 0)
    {
        // Make sure different commissioners run on different ports.
        port = static_cast<uint16_t>(port + CurrentCommissionerId());
    }
    factoryInitParams.listenPort = port;
    ReturnLogErrorOnFailure(DeviceControllerFactory::GetInstance().Init(factoryInitParams));

    const chip::Credentials::AttestationTrustStore * trustStore = mPaaTrustStorePath.HasValue()
        ? GetTestFileAttestationTrustStore(mPaaTrustStorePath.Value())
        : chip::Credentials::GetTestAttestationTrustStore();
    ;
    if (mPaaTrustStorePath.HasValue() && trustStore == nullptr)
    {
        ChipLogError(chipTool, "No PAAs found in path: %s", mPaaTrustStorePath.Value());
        ChipLogError(chipTool,
                     "Please specify a valid path containing trusted PAA certificates using [--paa-trust-store-path paa/file/path] "
                     "argument");

        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    ReturnLogErrorOnFailure(InitializeCommissioner(kIdentityNull, kIdentityNullFabricId, trustStore));
    ReturnLogErrorOnFailure(InitializeCommissioner(kIdentityAlpha, kIdentityAlphaFabricId, trustStore));
    ReturnLogErrorOnFailure(InitializeCommissioner(kIdentityBeta, kIdentityBetaFabricId, trustStore));
    ReturnLogErrorOnFailure(InitializeCommissioner(kIdentityGamma, kIdentityGammaFabricId, trustStore));

    // Initialize Group Data
    ReturnLogErrorOnFailure(chip::GroupTesting::InitProvider(mDefaultStorage));
    for (auto it = mCommissioners.begin(); it != mCommissioners.end(); it++)
    {
        chip::FabricInfo * fabric = it->second->GetFabricInfo();
        if ((nullptr != fabric) && (0 != it->first.compare(kIdentityNull)))
        {
            uint8_t compressed_fabric_id[sizeof(uint64_t)];
            chip::MutableByteSpan compressed_fabric_id_span(compressed_fabric_id);
            ReturnLogErrorOnFailure(fabric->GetCompressedId(compressed_fabric_id_span));
            ReturnLogErrorOnFailure(chip::GroupTesting::InitData(fabric->GetFabricIndex(), compressed_fabric_id_span));
        }
    }
    chip::DeviceLayer::PlatformMgr().ScheduleWork(RunQueuedCommand, reinterpret_cast<intptr_t>(this));
    CHIP_ERROR err = StartWaiting(GetWaitDuration());

    Shutdown();

    //
    // We can call DeviceController::Shutdown() safely without grabbing the stack lock
    // since the CHIP thread and event queue have been stopped, preventing any thread
    // races.
    //
    ReturnLogErrorOnFailure(ShutdownCommissioner(kIdentityNull));
    ReturnLogErrorOnFailure(ShutdownCommissioner(kIdentityAlpha));
    ReturnLogErrorOnFailure(ShutdownCommissioner(kIdentityBeta));
    ReturnLogErrorOnFailure(ShutdownCommissioner(kIdentityGamma));

    StopTracing();
    return err;
}

void CHIPCommand::StartTracing()
{
#if CHIP_CONFIG_TRANSPORT_TRACE_ENABLED
    chip::trace::InitTrace();

    if (mTraceFile.HasValue())
    {
        chip::trace::SetTraceStream(new chip::trace::TraceStreamFile(mTraceFile.Value()));
    }
    else if (mTraceLog.HasValue() && mTraceLog.Value())
    {
        chip::trace::SetTraceStream(new chip::trace::TraceStreamLog());
    }
#endif // CHIP_CONFIG_TRANSPORT_TRACE_ENABLED
}

void CHIPCommand::StopTracing()
{
#if CHIP_CONFIG_TRANSPORT_TRACE_ENABLED
    chip::trace::DeInitTrace();
#endif // CHIP_CONFIG_TRANSPORT_TRACE_ENABLED
}

void CHIPCommand::SetIdentity(const char * identity)
{
    std::string name = std::string(identity);
    if (name.compare(kIdentityAlpha) != 0 && name.compare(kIdentityBeta) != 0 && name.compare(kIdentityGamma) != 0 &&
        name.compare(kIdentityNull) != 0)
    {
        ChipLogError(chipTool, "Unknown commissioner name: %s. Supported names are [%s, %s, %s]", name.c_str(), kIdentityAlpha,
                     kIdentityBeta, kIdentityGamma);
        chipDie();
    }

    mCommissionerName.SetValue(const_cast<char *>(identity));
}

std::string CHIPCommand::GetIdentity()
{
    std::string name = mCommissionerName.HasValue() ? mCommissionerName.Value() : kIdentityAlpha;
    if (name.compare(kIdentityAlpha) != 0 && name.compare(kIdentityBeta) != 0 && name.compare(kIdentityGamma) != 0 &&
        name.compare(kIdentityNull) != 0)
    {
        ChipLogError(chipTool, "Unknown commissioner name: %s. Supported names are [%s, %s, %s]", name.c_str(), kIdentityAlpha,
                     kIdentityBeta, kIdentityGamma);
        chipDie();
    }

    return name;
}

chip::FabricId CHIPCommand::CurrentCommissionerId()
{
    chip::FabricId id;

    std::string name = GetIdentity();
    if (name.compare(kIdentityAlpha) == 0)
    {
        id = kIdentityAlphaFabricId;
    }
    else if (name.compare(kIdentityBeta) == 0)
    {
        id = kIdentityBetaFabricId;
    }
    else if (name.compare(kIdentityGamma) == 0)
    {
        id = kIdentityGammaFabricId;
    }
    else if (name.compare(kIdentityNull) == 0)
    {
        id = kIdentityNullFabricId;
    }
    else
    {
        VerifyOrDieWithMsg(false, chipTool, "Unknown commissioner name: %s. Supported names are [%s, %s, %s]", name.c_str(),
                           kIdentityAlpha, kIdentityBeta, kIdentityGamma);
    }

    return id;
}

chip::Controller::DeviceCommissioner & CHIPCommand::CurrentCommissioner()
{
    auto item = mCommissioners.find(GetIdentity());
    return *item->second.get();
}

CHIP_ERROR CHIPCommand::ShutdownCommissioner(std::string key)
{
    return mCommissioners[key].get()->Shutdown();
}

CHIP_ERROR CHIPCommand::InitializeCommissioner(std::string key, chip::FabricId fabricId,
                                               const chip::Credentials::AttestationTrustStore * trustStore)
{
    chip::Platform::ScopedMemoryBuffer<uint8_t> noc;
    chip::Platform::ScopedMemoryBuffer<uint8_t> icac;
    chip::Platform::ScopedMemoryBuffer<uint8_t> rcac;

    std::unique_ptr<ChipDeviceCommissioner> commissioner = std::make_unique<ChipDeviceCommissioner>();
    chip::Controller::SetupParams commissionerParams;

    ReturnLogErrorOnFailure(mCredIssuerCmds->SetupDeviceAttestation(commissionerParams, trustStore));
    chip::Credentials::SetDeviceAttestationVerifier(commissionerParams.deviceAttestationVerifier);

    VerifyOrReturnError(noc.Alloc(chip::Controller::kMaxCHIPDERCertLength), CHIP_ERROR_NO_MEMORY);
    VerifyOrReturnError(icac.Alloc(chip::Controller::kMaxCHIPDERCertLength), CHIP_ERROR_NO_MEMORY);
    VerifyOrReturnError(rcac.Alloc(chip::Controller::kMaxCHIPDERCertLength), CHIP_ERROR_NO_MEMORY);

    chip::Crypto::P256Keypair ephemeralKey;

    if (fabricId != chip::kUndefinedFabricId)
    {

        // TODO - OpCreds should only be generated for pairing command
        //        store the credentials in persistent storage, and
        //        generate when not available in the storage.
        ReturnLogErrorOnFailure(mCommissionerStorage.Init(key.c_str()));
        ReturnLogErrorOnFailure(mCredIssuerCmds->InitializeCredentialsIssuer(mCommissionerStorage));

        chip::MutableByteSpan nocSpan(noc.Get(), chip::Controller::kMaxCHIPDERCertLength);
        chip::MutableByteSpan icacSpan(icac.Get(), chip::Controller::kMaxCHIPDERCertLength);
        chip::MutableByteSpan rcacSpan(rcac.Get(), chip::Controller::kMaxCHIPDERCertLength);

        ReturnLogErrorOnFailure(ephemeralKey.Initialize());
        ReturnLogErrorOnFailure(mCredIssuerCmds->GenerateControllerNOCChain(mCommissionerStorage.GetLocalNodeId(), fabricId,
                                                                            mCommissionerStorage.GetCommissionerCATs(),
                                                                            ephemeralKey, rcacSpan, icacSpan, nocSpan));
        commissionerParams.operationalKeypair = &ephemeralKey;
        commissionerParams.controllerRCAC     = rcacSpan;
        commissionerParams.controllerICAC     = icacSpan;
        commissionerParams.controllerNOC      = nocSpan;
    }

    commissionerParams.storageDelegate                = &mCommissionerStorage;
    commissionerParams.operationalCredentialsDelegate = mCredIssuerCmds->GetCredentialIssuer();
    commissionerParams.controllerVendorId             = chip::VendorId::TestVendor1;

    ReturnLogErrorOnFailure(DeviceControllerFactory::GetInstance().SetupCommissioner(commissionerParams, *(commissioner.get())));
    mCommissioners[key] = std::move(commissioner);

    return CHIP_NO_ERROR;
}

void CHIPCommand::RunQueuedCommand(intptr_t commandArg)
{
    auto * command = reinterpret_cast<CHIPCommand *>(commandArg);
    CHIP_ERROR err = command->RunCommand();
    if (err != CHIP_NO_ERROR)
    {
        command->SetCommandExitStatus(err);
    }
}

#if !CONFIG_USE_SEPARATE_EVENTLOOP
static void OnResponseTimeout(chip::System::Layer *, void * appState)
{
    (reinterpret_cast<CHIPCommand *>(appState))->SetCommandExitStatus(CHIP_ERROR_TIMEOUT);
}
#endif // !CONFIG_USE_SEPARATE_EVENTLOOP

CHIP_ERROR CHIPCommand::StartWaiting(chip::System::Clock::Timeout duration)
{
#if CONFIG_USE_SEPARATE_EVENTLOOP
    // ServiceEvents() calls StartEventLoopTask(), which is paired with the StopEventLoopTask() below.
    ReturnLogErrorOnFailure(DeviceControllerFactory::GetInstance().ServiceEvents());
    auto waitingUntil = std::chrono::system_clock::now() + std::chrono::duration_cast<std::chrono::seconds>(duration);
    {
        std::unique_lock<std::mutex> lk(cvWaitingForResponseMutex);
        if (!cvWaitingForResponse.wait_until(lk, waitingUntil, [this]() { return !this->mWaitingForResponse; }))
        {
            mCommandExitStatus = CHIP_ERROR_TIMEOUT;
        }
    }
    LogErrorOnFailure(chip::DeviceLayer::PlatformMgr().StopEventLoopTask());
#else
    ReturnLogErrorOnFailure(chip::DeviceLayer::SystemLayer().StartTimer(duration, OnResponseTimeout, this));
    chip::DeviceLayer::PlatformMgr().RunEventLoop();
#endif // CONFIG_USE_SEPARATE_EVENTLOOP

    return mCommandExitStatus;
}

void CHIPCommand::StopWaiting()
{
#if CONFIG_USE_SEPARATE_EVENTLOOP
    {
        std::lock_guard<std::mutex> lk(cvWaitingForResponseMutex);
        mWaitingForResponse = false;
    }
    cvWaitingForResponse.notify_all();
#else  // CONFIG_USE_SEPARATE_EVENTLOOP
    LogErrorOnFailure(chip::DeviceLayer::PlatformMgr().StopEventLoopTask());
#endif // CONFIG_USE_SEPARATE_EVENTLOOP
}
