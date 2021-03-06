#include "stdafx.h"
#include "pgkd.h"
#include "Progress.h"
#include "PoolTagNote.h"
#include "scope_guard.h"

#include <sstream>
#include <iomanip>

// EXT_DECLARE_GLOBALS must be used to instantiate
// the framework's assumed globals.
EXT_DECLARE_GLOBALS();

extern"C" auto _EFN_Analyze(
    PDEBUG_CLIENT4            aClient,
    FA_EXTENSION_PLUGIN_PHASE aCallPhase,
    PDEBUG_FAILURE_ANALYSIS2  aAnalysis)
    ->HRESULT
{
    return g_ExtInstance._EFN_Analyze(aClient, aCallPhase, aAnalysis);
}

namespace Sunstrider
{

    // Exported command !findpg (no options)
    EXT_COMMAND(findpg, 
        "Displays base addresses of PatchGuard pages",
        "")
    {
        try
        {
            FindPatchGuardContext();
        }
        catch (std::exception& aWhat)
        {
            // As an exception string does not appear on Windbg,
            // we need to handle it manually.
            Err("%s\n", aWhat.what());
        }
    }

    // Exported command !analyzepg (no options)
    EXT_COMMAND(analyzepg,
        "Analyzes current bugcheck 0x109: CRITICAL_STRUCTURE_CORRUPTION",
        "")
    {
        for (;;)
        {
            if (IsCurMachine32())
            {
                Err("!analyzepg not support x86 target!\n");
                break;
            }

            auto    vBugCheckCode = 0ul;
            UINT64  vBugCheckArgs[4]{};
            auto hr = m_Control->ReadBugCheckData(
                &vBugCheckCode,
                &vBugCheckArgs[0],
                &vBugCheckArgs[1],
                &vBugCheckArgs[2],
                &vBugCheckArgs[3]);
            if (FAILED(hr) || vBugCheckCode != 0x109)
            {
                Err("No CRITICAL_STRUCTURE_CORRUPTION bugcheck information was"
                    " derived.\n");
                break;
            }

            auto vPGContext = vBugCheckArgs[0] - BUGCHECK_109_ARGS0_KEY;
            auto vPGReason  = vBugCheckArgs[1] ? vBugCheckArgs[1] - BUGCHECK_109_ARGS1_KEY : 0;

            DumpPatchGuard(
                vPGContext,
                vPGReason,
                vBugCheckArgs[2],
                vBugCheckArgs[3],
                true);

            break;
        }
    }

    // Exported command !dumppg <address>
    EXT_COMMAND(dumppg,
        "Displays the PatchGuard context",
        "{;e64;address;An address of a PatchGuard context.}")
    {
        DumpPatchGuard(GetUnnamedArgU64(0), 0, 0, 0);
    }

    auto PGKd::_EFN_Analyze(
        PDEBUG_CLIENT4              aClient, 
        FA_EXTENSION_PLUGIN_PHASE   aCallPhase,
        PDEBUG_FAILURE_ANALYSIS2    /*aAnalysis*/) 
        -> HRESULT
    {
        HRESULT hr = S_OK;

        if (FA_PLUGIN_POST_BUCKETING == aCallPhase)
        {
            if (!g_Ext.IsSet())
            {
                return E_UNEXPECTED;
            }
            return g_Ext->CallCommand(&g_analyzepgDesc, (PDEBUG_CLIENT)aClient, "");
        }

        return hr;
    }

    auto PGKd::Initialize()
        -> HRESULT
    {
        HRESULT hr = S_OK;

        for (;;)
        {
            auto vDbgClient = static_cast<IDebugClient*>(nullptr);
            hr = DebugCreate(__uuidof(IDebugClient), (void **)&vDbgClient);
            if (FAILED(hr))
            {
                break;
            }
            auto vDbgClientScope = std::experimental::scope_guard(
                vDbgClient, [](IDebugClient* aDbgClient){ aDbgClient->Release(); });

            auto vDbgControl = static_cast<IDebugControl*>(nullptr);
            hr = vDbgClient->QueryInterface(__uuidof(IDebugControl), (void **)&vDbgControl);
            if (FAILED(hr))
            {
                break;
            }
            auto vDbgControlScope = std::experimental::scope_guard(
                vDbgControl, [](IDebugControl* aDbgControl){ aDbgControl->Release(); });

            ExtensionApis.nSize = sizeof(ExtensionApis);
            hr = vDbgControl->GetWindbgExtensionApis64(&ExtensionApis);
            if (FAILED(hr))
            {
                break;
            }

            auto vTarget = std::string();
            if (wdk::SystemVersion::Unknown == GetSystemVersion(vDbgControl, &vTarget))
            {
                dprintf("Unsupported version detected: %s\n", vTarget.c_str());
                hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            }

            // Display guide messages
            dprintf("Use ");
            vDbgControl->ControlledOutput(
                DEBUG_OUTCTL_AMBIENT_DML, DEBUG_OUTPUT_NORMAL,
                "<exec cmd=\"!findpg\">!findpg</exec>");
            dprintf(" to find base addresses of the pages allocated for PatchGuard.\n");

            dprintf("Use ");
            vDbgControl->ControlledOutput(
                DEBUG_OUTCTL_AMBIENT_DML, DEBUG_OUTPUT_NORMAL,
                "<exec cmd=\"!analyzepg\">!analyzepg</exec>");
            dprintf(" to get detailed PatchGuard Bugcheck information.\n");

            dprintf("Use !dumppg <address> to display PatchGuard information"
                " located at the specific address.\n");
            dprintf("\n");

            break;
        }

        return hr;
    }
    
    auto PGKd::GetSystemVersion(PDEBUG_CONTROL aDbgControl, std::string* aTarget)
        -> wdk::SystemVersion
    {
        static auto sSystemVersion = wdk::SystemVersion::Unknown;

        if (wdk::SystemVersion::Unknown != sSystemVersion)
        {
            return sSystemVersion;
        }

        for (;;)
        {
            if (nullptr == aDbgControl)
            {
                break;
            }

            auto vPlatformId = 0ul;
            auto vMajorVer   = 0ul;
            auto vMinorVer   = 0ul;
            auto vServicePackNumber = 0ul;
            
            auto vBuildBuffer = std::make_unique<std::array<char, 1024>>();
            auto hr = aDbgControl->GetSystemVersion(
                &vPlatformId,
                &vMajorVer,
                &vMinorVer,
                nullptr, 0, nullptr,
                &vServicePackNumber,
                vBuildBuffer->data(), static_cast<ULONG>(vBuildBuffer->size()),
                nullptr);
            if (FAILED(hr))
            {
                break;
            }

            const auto vBuild = std::string(vBuildBuffer->data());

            if (aTarget) *aTarget = vBuild;

            // Check if the target platform is supported
            struct VersionMap
            {
                std::string         Build;
                wdk::SystemVersion  Version;
            };

            const VersionMap sSupportedVersions[] =
            {
                { "Built by: 3790.", wdk::SystemVersion::WindowsXP64, },
                { "Built by: 6000.", wdk::SystemVersion::WindowsVista, },
                { "Built by: 6001.", wdk::SystemVersion::WindowsVista_SP1, },
                { "Built by: 6002.", wdk::SystemVersion::WindowsVista_SP2, },
                { "Built by: 7600.", wdk::SystemVersion::Windows7, },
                { "Built by: 7601.", wdk::SystemVersion::Windows7_SP1, },
                { "Built by: 9200.", wdk::SystemVersion::Windows8, },
                { "Built by: 9600.", wdk::SystemVersion::Windows8_1, },
                { "Built by: 10240.", wdk::SystemVersion::Windows10_1507, },
                { "Built by: 10586.", wdk::SystemVersion::Windows10_1511, },
                { "Built by: 14393.", wdk::SystemVersion::Windows10_1607, },
                { "Built by: 15063.", wdk::SystemVersion::Windows10_1703, },
                { "Built by: 16299.", wdk::SystemVersion::Windows10_1709, },
                { "Built by: 17134.", wdk::SystemVersion::Windows10_1803, },
            };

            for (const auto& vItem : sSupportedVersions)
            {
                if (vBuild.substr(0, vItem.Build.length()) == vItem.Build)
                {
                    sSystemVersion = vItem.Version;
                    break;
                }
            }

            break;
        }

        return sSystemVersion;
    }

    auto PGKd::IsWindows10OrGreater()
        -> bool
    {
        return (GetSystemVersion() >= wdk::SystemVersion::Windows10);
    }

    auto PGKd::IsWindowsRS1OrGreater() 
        -> bool
    {
        return (GetSystemVersion() >= wdk::SystemVersion::Windows10_1607);
    }

    auto PGKd::GetPfnDatabase() 
        -> UINT64
    {
        static UINT64 sPfnDatabase = 0;
        if (sPfnDatabase)
        {
            return sPfnDatabase;
        }

        for (;;)
        {
            auto vOffset = 0UI64;
            auto hr = m_Symbols->GetOffsetByName("nt!MmPfnDatabase", &vOffset);
            if (FAILED(hr))
            {
                throw std::runtime_error("nt!MmPfnDatabase could not be found.");
                break;
            }

            hr = m_Data->ReadPointersVirtual(1, vOffset, &sPfnDatabase);
            if (FAILED(hr))
            {
                throw std::runtime_error("nt!MmGetVirtualForPhysical could not be read PTE_BASE.");
                break;
            }

            break;
        }

        return sPfnDatabase;
    }

    auto PGKd::GetPteBase() 
        -> UINT64
    {
        static UINT64 sPteBase = 0;
        if (sPteBase)
        {
            return sPteBase;
        }

        for (;;)
        {
            if (!IsWindowsRS1OrGreater())
            {
                sPteBase = 0xFFFFF68000000000UI64;
                break;
            }

            auto vOffset = 0UI64;
            auto hr = m_Symbols->GetOffsetByName("nt!MmGetVirtualForPhysical", &vOffset);
            if (FAILED(hr))
            {
                throw std::runtime_error("nt!MmGetVirtualForPhysical could not be found.");
                break;
            }

            static UINT8 sSearchPatten[] =
            {
                // mov     rax,[rax + rdx * 8]
                // shl     rax, 19h
                // mov     rdx, PTE_BASE

                0x48, 0x8B, 0x04, 0xD0,
                0x48, 0xC1, 0xE0, 0x19,
                0x48, 0xBA, // 00 00 00 00 80 F6 FF FF
            };

            hr = m_Data->SearchVirtual(vOffset, 0x60, sSearchPatten, sizeof(sSearchPatten), 1, &vOffset);
            if (!SUCCEEDED(hr))
            {
                throw std::runtime_error("nt!MmGetVirtualForPhysical could not be search PTE_BASE.");
                break;
            }

            hr = m_Data->ReadPointersVirtual(1, vOffset + sizeof(sSearchPatten), &sPteBase);
            if (FAILED(hr))
            {
                throw std::runtime_error("nt!MmGetVirtualForPhysical could not be read PTE_BASE.");
                break;
            }
            
            break;
        }

        return sPteBase;
    }

    auto PGKd::GetPtes(UINT64 aPteBase)
        -> std::unique_ptr<std::array<wdk::HARDWARE_PTE, wdk::PTE_PER_PAGE>>
    {
        auto vPtes = std::make_unique<std::array<wdk::HARDWARE_PTE, wdk::PTE_PER_PAGE>>();

        auto vReadBytes = 0ul;
        if (FAILED(m_Data->ReadVirtual(
            aPteBase,
            vPtes->data(), static_cast<ULONG>(vPtes->size() * sizeof(wdk::HARDWARE_PTE)), 
            &vReadBytes)))
        {
            throw std::runtime_error("The given Ptes address could not be read.");
        }

        return std::move(vPtes);
    }

    auto PGKd::IsNonPagedBigPool(const wdk::POOL_TRACKER_BIG_PAGES & aEntry)
        -> bool
    {
        auto vPoolType = wdk::POOL_TYPE::PagedPool;

        if (IsWindows10OrGreater())
        {
            auto vEntry = (wdk::build_10240::POOL_TRACKER_BIG_PAGES*)&aEntry;
            vPoolType   = (wdk::POOL_TYPE)vEntry->PoolType;
        }
        else
        {
            auto vEntry = (wdk::POOL_TRACKER_BIG_PAGES*)&aEntry;
            vPoolType   = (wdk::POOL_TYPE)vEntry->PoolType;
        }

        bool vNonPaged = false;
        switch (vPoolType)
        {
        default:
            break;

        case wdk::POOL_TYPE::NonPagedPool:
        case wdk::POOL_TYPE::NonPagedPoolCacheAligned:
        case wdk::POOL_TYPE::NonPagedPoolCacheAlignedMustS:
        case wdk::POOL_TYPE::NonPagedPoolCacheAlignedSession:
        case wdk::POOL_TYPE::NonPagedPoolMustSucceed:
        case wdk::POOL_TYPE::NonPagedPoolMustSucceedSession:
        case wdk::POOL_TYPE::NonPagedPoolNx:
        case wdk::POOL_TYPE::NonPagedPoolNxCacheAligned:
        case wdk::POOL_TYPE::NonPagedPoolSession:
        case wdk::POOL_TYPE::NonPagedPoolSessionNx:
            vNonPaged = true;
            break;
        }

        return vNonPaged;
    }
    
    // Returns true when page protection of the given page is
    // Readable/Writable/Executable.
    auto PGKd::IsPageValidReadWriteExecutable(UINT64 aPteAddress) -> bool
    {
        auto vReadBytes = 0ul;
        auto vPte       = wdk::HARDWARE_PTE{};

        auto hr = m_Data->ReadVirtual(aPteAddress, &vPte, sizeof(vPte), &vReadBytes);
        if (FAILED(hr))
        {
            return false;
        }

        return (vPte.Valid && vPte.Write && !vPte.NoExecute);
    }

    // Returns true when page protection of the given page or a parant page
    // of the given page is Valid and Readable/Writable/Executable.
    auto PGKd::IsPatchGuardPageAttribute(UINT64 aPageBase) 
        -> bool
    {
        const auto vPte = wdk::MiAddressToPte(reinterpret_cast<void*>(aPageBase));
        if (IsPageValidReadWriteExecutable(reinterpret_cast<ULONG64>(vPte)))
        {
            return true;
        }
        const auto vPde = wdk::MiAddressToPde(reinterpret_cast<void*>(aPageBase));
        if (IsPageValidReadWriteExecutable(reinterpret_cast<ULONG64>(vPde)))
        {
            return true;
        }
        return false;
    }

    auto PGKd::FindPatchGuardContextFromBigPagePool() 
        -> std::vector<std::tuple<wdk::POOL_TRACKER_BIG_PAGES, RandomnessInfo>>
    {
        auto vResult = std::vector<std::tuple<wdk::POOL_TRACKER_BIG_PAGES, RandomnessInfo>>();

        for (;;)
        {
            auto vOffset = 0UI64;
            auto hr = m_Symbols->GetOffsetByName("nt!PoolBigPageTableSize", &vOffset);
            if (FAILED(hr))
            {
                throw std::runtime_error("nt!PoolBigPageTableSize could not be found.");
                break;
            }

            SIZE_T PoolBigPageTableSize = 0;
            hr = m_Data->ReadPointersVirtual(1, vOffset, &PoolBigPageTableSize);
            if (FAILED(hr))
            {
                throw std::runtime_error("nt!PoolBigPageTableSize could not be read.");
                break;
            }

            // Read PoolBigPageTable
            hr = m_Symbols->GetOffsetByName("nt!PoolBigPageTable", &vOffset);
            if (FAILED(hr))
            {
                throw std::runtime_error("nt!PoolBigPageTable could not be found.");
                break;
            }

            auto PoolBigPageTable = 0UI64;
            hr = m_Data->ReadPointersVirtual(1, vOffset, &PoolBigPageTable);
            if (FAILED(hr))
            {
                throw std::runtime_error("nt!PoolBigPageTable could not be read.");
                break;
            }

            auto vReadBytes = 0ul;
            auto vTable = std::vector<wdk::POOL_TRACKER_BIG_PAGES>(PoolBigPageTableSize);
            hr = m_Data->ReadVirtual(
                PoolBigPageTable,
                vTable.data(), static_cast<ULONG>(vTable.size() * sizeof(wdk::POOL_TRACKER_BIG_PAGES)),
                &vReadBytes);
            if (FAILED(hr))
            {
                throw std::runtime_error("nt!PoolBigPageTable could not be read.");
                break;
            }

            // Walk BigPageTable
            Progress vProgress(this);
            for (SIZE_T i = 0; i < PoolBigPageTableSize; ++i)
            {
                if ((i % 0x1000) == 0)
                {
                    ++vProgress;
                }

                const auto& vEntry = vTable[i];
                auto vStartAddress = reinterpret_cast<ULONG_PTR>(vEntry.Va);

                // Ignore unused entries
                if (!vStartAddress || (vStartAddress & 1))
                {
                    continue;
                }

                // Filter by the size of region
                if (MINIMUM_REGION_SIZE  > vEntry.NumberOfBytes || 
                    vEntry.NumberOfBytes > MAXIMUM_REGION_SIZE)
                {
                    continue;
                }

                if (!IsNonPagedBigPool(vEntry))
                {
                    continue;
                }

                // Filter by the page protection
                if (!IsPatchGuardPageAttribute(vStartAddress))
                {
                    continue;
                }

                // Read and check randomness of the contents
                auto vContents = std::make_unique<std::array<UINT8, EXAMINATION_BYTES>>();
                hr = m_Data->ReadVirtual(vStartAddress,
                    vContents->data(), static_cast<ULONG>(vContents->size()), &vReadBytes);
                if (FAILED(hr))
                {
                    continue;
                }

                const auto vNumberOfDistinctiveNumbers = GetNumberOfDistinctiveNumbers(
                    vContents->data(), EXAMINATION_BYTES);
                const auto vRandomness = GetRamdomness(vContents->data(), EXAMINATION_BYTES);

                if (vNumberOfDistinctiveNumbers > MAXIMUM_DISTINCTIVE_NUMBER ||
                    vRandomness < MINIMUM_RANDOMNESS)
                {
                    continue;
                }

                // It seems to be a PatchGuard page
                vResult.emplace_back(vEntry,
                    RandomnessInfo{ vNumberOfDistinctiveNumbers, vRandomness, });
            }

            break;
        }

        return std::move(vResult);
    }

    auto PGKd::FindPatchGuardContextFromIndependentPages() 
        -> std::vector<std::tuple<UINT64, SIZE_T, RandomnessInfo>>
    {
        auto vResult = std::vector<std::tuple<UINT64, SIZE_T, RandomnessInfo>>();

        for (;;)
        {
            auto vOffset = 0UI64;
            auto hr = m_Symbols->GetOffsetByName("nt!MmSystemRangeStart", &vOffset);
            if (FAILED(hr))
            {
                throw std::runtime_error("nt!MmSystemRangeStart could not be found.");
                break;
            }

            auto MmSystemRangeStart = 0UI64;
            hr = m_Data->ReadPointersVirtual(1, vOffset, &MmSystemRangeStart);
            if (FAILED(hr))
            {
                throw std::runtime_error("nt!MmSystemRangeStart could not be read.");
                break;
            }

            Progress vProgress(this);
            
            // Walk entire page table (PXE -> PPE -> PDE -> PTE)
            // Start parse PXE (PML4) which represents the beginning of kernel address
            const auto vStartPxe = reinterpret_cast<UINT64>(
                wdk::MiAddressToPxe(reinterpret_cast<void*>(MmSystemRangeStart)));
            const auto vEndPxe   = wdk::PXE_TOP;
            const auto vPxes     = GetPtes(wdk::PXE_BASE);

            for (auto vCurrentPxe = vStartPxe; vCurrentPxe < vEndPxe; vCurrentPxe += sizeof(wdk::HARDWARE_PTE))
            {
                // Make sure that this PXE is valid
                const auto vPxeIndex = (vCurrentPxe - wdk::PXE_BASE) / sizeof(wdk::HARDWARE_PTE);
                const auto vPxe = (*vPxes)[vPxeIndex];
                if (!vPxe.Valid)
                {
                    continue;
                }

                // If the PXE is valid, analyze PPE belonging to this
                const auto vStartPpe = wdk::PPE_BASE + 0x1000 * vPxeIndex;
                const auto vEndPpe   = wdk::PPE_BASE + 0x1000 * (vPxeIndex + 1);
                const auto vPpes     = GetPtes(vStartPpe);

                for (auto vCurrentPpe = vStartPpe; vCurrentPpe < vEndPpe; vCurrentPpe += sizeof(wdk::HARDWARE_PTE))
                {
                    // Make sure that this PPE is valid
                    const auto vPpeIndex1 = (vCurrentPpe - wdk::PPE_BASE) / sizeof(wdk::HARDWARE_PTE);
                    const auto vPpeIndex2 = (vCurrentPpe - vStartPpe) / sizeof(wdk::HARDWARE_PTE);
                    const auto vPpe = (*vPpes)[vPpeIndex2];
                    if (!vPpe.Valid)
                    {
                        continue;
                    }

                    // If the PPE is valid, analyze PDE belonging to this
                    const auto vStartPde = wdk::PDE_BASE + 0x1000 * vPpeIndex1;
                    const auto vEndPde   = wdk::PDE_BASE + 0x1000 * (vPpeIndex1 + 1);
                    const auto vPdes     = GetPtes(vStartPde);

                    for (auto vCurrentPde = vStartPde; vCurrentPde < vEndPde; vCurrentPde += sizeof(wdk::HARDWARE_PTE))
                    {
                        // Make sure that this PDE is valid as well as is not handling
                        // a large page as an independent page does not use a large page
                        const auto vPdeIndex1 = (vCurrentPde - wdk::PDE_BASE) / sizeof(wdk::HARDWARE_PTE);
                        const auto vPdeIndex2 = (vCurrentPde - vStartPde) / sizeof(wdk::HARDWARE_PTE);
                        const auto vPde = (*vPdes)[vPdeIndex2];
                        if (!vPde.Valid || vPde.LargePage)
                        {
                            continue;
                        }

                        ++vProgress;

                        // If the PDE is valid, analyze PTE belonging to this
                        const auto vStartPte = wdk::PTE_BASE + 0x1000 * vPdeIndex1;
                        const auto vEndPte   = wdk::PTE_BASE + 0x1000 * (vPdeIndex1 + 1);
                        const auto vPtes     = GetPtes(vStartPte);

                        for (auto vCurrentPte = vStartPte; vCurrentPte < vEndPte; vCurrentPte += sizeof(wdk::HARDWARE_PTE))
                        {
                            // Make sure that this PPE is valid,
                            // Readable/Writable/Executable
                            const auto vPteIndex2 = (vCurrentPte - vStartPte) / sizeof(wdk::HARDWARE_PTE);
                            const auto vPte = (*vPtes)[vPteIndex2];
                            if (!vPte.Valid ||
                                !vPte.Write ||
                                vPte.NoExecute)
                            {
                                continue;
                            }

                            // This page might be PatchGuard page, so let's analyze it
                            const auto vVirtualAddress = reinterpret_cast<ULONG64>(
                                wdk::MiPteToAddress(reinterpret_cast<wdk::HARDWARE_PTE*>(vCurrentPte))) 
                                | 0xffff000000000000;

                            // Read the contents of the address that is managed by the
                            // PTE
                            auto vReadBytes = 0ul;
                            auto vContents  = std::make_unique<std::array<std::uint8_t, EXAMINATION_BYTES + sizeof(ULONG64)>>();

                            hr = m_Data->ReadVirtual(
                                vVirtualAddress,
                                vContents->data(), static_cast<ULONG>(vContents->size()),
                                &vReadBytes);
                            if (FAILED(hr))
                            {
                                continue;
                            }

                            // Check randomness of the contents
                            const auto vNumberOfDistinctiveNumbers =
                                GetNumberOfDistinctiveNumbers(vContents->data() + sizeof(ULONG64), EXAMINATION_BYTES);
                            const auto vRandomness = 
                                GetRamdomness(vContents->data() + sizeof(ULONG64), EXAMINATION_BYTES);

                            if (vNumberOfDistinctiveNumbers > MAXIMUM_DISTINCTIVE_NUMBER || 
                                vRandomness < MINIMUM_RANDOMNESS)
                            {
                                continue;
                            }

                            // Also, check the size of the region. The first page of
                            // allocated pages as independent pages has its own page
                            // size in bytes at the first 8 bytes
                            const auto vIndependentPageSize =
                                *reinterpret_cast<ULONG64*>(vContents->data());
                            if (MINIMUM_REGION_SIZE > vIndependentPageSize
                                || vIndependentPageSize > MAXIMUM_REGION_SIZE)
                            {
                                continue;
                            }

                            // It seems to be a PatchGuard page
                            vResult.emplace_back(vVirtualAddress, vIndependentPageSize,
                                RandomnessInfo{ vNumberOfDistinctiveNumbers, vRandomness, });
                        }
                    }
                }
            }

            break;
        }

        return std::move(vResult);
    }

    auto PGKd::FindPatchGuardContext() 
        -> HRESULT
    {
        HRESULT hr = S_OK;

        for (;;)
        {
            if (IsCurMachine32())
            {
                throw std::runtime_error("!findpg not support x86 target!");
                break;
            }

            Out("Wait until analysis is completed. It typically takes 2-5 minutes.\n");
            Out("Or press Ctrl+Break or [Debug] > [Break] to stop analysis.\n");

            if (IsWindowsRS1OrGreater())
            {
                wdk::MiInitPte(GetPteBase());
            }

            // Collect PatchGuard pages from NonPagedPool and Independent pages
            auto vFoundBigPagePool = FindPatchGuardContextFromBigPagePool();
            Out("Phase 1 analysis has been done. [BigPagePool]\n");

            auto vFoundIndependent = FindPatchGuardContextFromIndependentPages();
            Out("Phase 2 analysis has been done. [IndependentPages]\n");

            // Sort data according to its base addresses
            std::sort(vFoundBigPagePool.begin(), vFoundBigPagePool.end(), 
                [](
                const decltype(vFoundBigPagePool)::value_type& Lhs,
                const decltype(vFoundBigPagePool)::value_type& Rhs)
            {
                return std::get<0>(Lhs).Va < std::get<0>(Rhs).Va;
            });
            std::sort(vFoundIndependent.begin(), vFoundIndependent.end(),
                [](
                const decltype(vFoundIndependent)::value_type& Lhs,
                const decltype(vFoundIndependent)::value_type& Rhs)
            {
                return std::get<0>(Lhs) < std::get<0>(Rhs);
            });

            // Display collected data
            PoolTagNote vPooltag(this);
            for (const auto& vItem : vFoundBigPagePool)
            {
                const auto vDescription = vPooltag.get(std::get<0>(vItem).Tag);

                Out("[BigPagePool] PatchGuard context page base: %y, size: 0x%08x,"
                    " Randomness %3d:%3d,%s\n",
                    std::get<0>(vItem).Va, std::get<0>(vItem).NumberOfBytes,
                    std::get<1>(vItem).NumberOfDistinctiveNumbers,
                    std::get<1>(vItem).Ramdomness,
                    vDescription.c_str());
            }
            for (const auto& vItem : vFoundIndependent)
            {
                Out("[Independent] PatchGuard context page base: %y, Size: 0x%08x,"
                    " Randomness %3d:%3d,\n",
                    std::get<0>(vItem), std::get<1>(vItem),
                    std::get<2>(vItem).NumberOfDistinctiveNumbers,
                    std::get<2>(vItem).Ramdomness);
            }
            
            break;
        }

        return hr;
    }

    auto PGKd::GetPGContextTypeString(
        UINT64 aErrorWasFound,
        UINT64 aTypeOfCorruption)
        -> LPCSTR
    {
        // Reference:
        // https://docs.microsoft.com/en-us/windows-hardware/drivers/debugger/bug-check-0x109---critical-structure-corruption

        if (!aErrorWasFound)
        {
            return "N/A";
        }

        switch (aTypeOfCorruption)
        {
        case 0x0000: return "A generic data region";
        case 0x0001: return "A function modification or the Itanium-based function location";
        case 0x0002: return "A processor interrupt dispatch table (IDT)";
        case 0x0003: return "A processor global descriptor table (GDT)";
        case 0x0004: return "A type-1 process list corruption";
        case 0x0005: return "A type-2 process list corruption";
        case 0x0006: return "A debug routine modification";
        case 0x0007: return "A critical MSR modification";
        case 0x0008: return "Object type";
        case 0x0009: return "A processor IVT";
        case 0x000A: return "Modification of a system service function";
        case 0x000B: return "A generic session data region";
        case 0x000C: return "Modification of a session function or .pdata";
        case 0x000D: return "Modification of an import table";
        case 0x000E: return "Modification of a session import table";
        case 0x000F: return "Ps Win32 callout modification";
        case 0x0010: return "Debug switch routine modification";
        case 0x0011: return "IRP allocator modification";
        case 0x0012: return "Driver call dispatcher modification";
        case 0x0013: return "IRP completion dispatcher modification";
        case 0x0014: return "IRP deallocator modification";
        case 0x0015: return "A processor control register";
        case 0x0016: return "Critical floating point control register modification";
        case 0x0017: return "Local APIC modification";
        case 0x0018: return "Kernel notification callout modification";
        case 0x0019: return "Loaded module list modification";
        case 0x001A: return "Type 3 process list corruption";
        case 0x001B: return "Type 4 process list corruption";
        case 0x001C: return "Driver object corruption";
        case 0x001D: return "Executive callback object modification";
        case 0x001E: return "Modification of module padding";
        case 0x001F: return "Modification of a protected process";
        case 0x0020: return "A generic data region";
        case 0x0021: return "A page hash mismatch";
        case 0x0022: return "A session page hash mismatch";
        case 0x0023: return "Load config directory modification";
        case 0x0024: return "Inverted function table modification";
        case 0x0025: return "Session configuration modification";
        case 0x0026: return "An extended processor control register";
        case 0x0027: return "Type 1 pool corruption";
        case 0x0028: return "Type 2 pool corruption";
        case 0x0029: return "Type 3 pool corruption";
        case 0x002A: return "Type 4 pool corruption";
        case 0x002B: return "Modification of a function or .pdata";
        case 0x002C: return "Image integrity corruption";
        case 0x002D: return "Processor misconfiguration";
        case 0x002E: return "Type 5 process list corruption";
        case 0x002F: return "Process shadow corruption";
        case 0x0101: return "General pool corruption";
        case 0x0102: return "Modification of win32k.sys";
        case 0x0103: return "MmAttachSession failure";
        case 0x0104: return "KeInsertQueueApc failure";
        case 0x0105: return "RtlImageNtHeader failure";
        case 0x0106: return "CcBcbProfiler detected modification";
        case 0x0107: return "KiTableInformation corruption";
        case 0x0108: return "Not investigated :(";
        case 0x0109: return "Type 2 context modification";
        case 0x010E: return "Inconsistency between before and after sleeping";
        default: break;
        }

        return "Unknown :(";
    }

    auto PGKd::DumpPatchGuardContextForType106(
        UINT64 aFailureDependent)
        -> void
    {
        static const char DUMP_FORMAT[] = R"RAW(
    PatchGuard Context      : %y, An address of PatchGuard context
    Validation Data         : %y, An address of validation data that caused the error
    Type of Corruption      : Available %I64d   : %I64x : %s
    Failure type dependent information      : %y

)RAW";
        static const auto TYPE_OF_CORRUPTION = 0x106ull;
        const auto vTypeString = GetPGContextTypeString(true, TYPE_OF_CORRUPTION);
        Out(DUMP_FORMAT,
            0ull,
            0ull,
            1ull, TYPE_OF_CORRUPTION, vTypeString,
            aFailureDependent);
    }

    template<>
    auto PGKd::DumpPatchGuardContext(
        UINT64 aPGContext, 
        UINT64 aPGReason, 
        UINT64 aFailureDependent,
        UINT64 aTypeOfCorruption, 
        wdk::build_7601::PGContext& aContext)
        -> HRESULT
    {
        static const char DUMP_FORMAT[] = R"RAW(
    PatchGuard Context      : %y, An address of PatchGuard context
    Validation Data         : %y, An address of validation data that caused the error
    Type of Corruption      : Available %I64d   : %I64x : %s
    Failure type dependent information      : %y
    Allocated memory base                   : %y
    Prcb                                    : %y 
    PGSelfValidation                        : %y
    RtlLookupFunctionEntryEx                : %y
    FsRtlUninitializeSmallMcb               : %y
    FsRtlMdlReadCompleteDevEx               : %y
    PGProtectCode2[?]                       : %y
    ContextSizeInBytes                      : %08x (Can be broken)
    DPC Routine                             : %y
    WorkerRoutine                           : %y
    Number Of Protected Codes               : %08x
    Number Of Protected Values              : %08x
    Protected Codes  Table                  : %y
    Protected Values Table                  : %y

)RAW";

        auto FsRtlUninitializeSmallMcb = 0I64;
        auto hr = m_Symbols->GetOffsetByName("nt!FsRtlUninitializeSmallMcb", (PUINT64)&FsRtlUninitializeSmallMcb);
        if (FAILED(hr))
        {
            return hr;
        }
        auto FsRtlMdlReadCompleteDevEx = 0I64;
        hr = m_Symbols->GetOffsetByName("nt!FsRtlMdlReadCompleteDevEx", (PUINT64)&FsRtlMdlReadCompleteDevEx);
        if (FAILED(hr))
        {
            return hr;
        }
        auto vFsRtlXXXOffset = FsRtlUninitializeSmallMcb - FsRtlMdlReadCompleteDevEx;

        auto vPGContext = aPGContext;
        auto vPGReason  = aPGReason;
        auto vFailureDependent = aFailureDependent;
        auto vTypeOfCorruption = aTypeOfCorruption;
        auto vPGSelfValidation = aPGContext + aContext.OffsetOfPGSelfValidation;
        auto vFsRtlUninitializeSmallMcb = aPGContext + aContext.OffsetOfFsRtlUninitializeSmallMcb;
        auto vFsRtlMdlReadCompleteDevEx = vFsRtlUninitializeSmallMcb - vFsRtlXXXOffset;
        auto vRtlLookupFunctionEntryEx  = aPGContext + aContext.OffsetOfRtlLookupFunctionEntryEx;
        auto vPGProtectCode2Table       = aContext.OffsetOfPGProtectCode2Table ? aPGContext + aContext.OffsetOfPGProtectCode2Table : 0;
        auto vProtectCodes  = aPGContext + sizeof(aContext);
        auto vProtectValues = vProtectCodes + (sizeof(wdk::PGProtectCode) * aContext.NumberOfProtectCodes);

        if (aContext.IsTiggerPG)
        {
            vPGContext = aContext.BugCheckArg0 - BUGCHECK_109_ARGS0_KEY;
            vPGReason  = aContext.BugCheckArg1 - BUGCHECK_109_ARGS1_KEY;
            vFailureDependent = aContext.BugCheckArg2;
            vTypeOfCorruption = aContext.BugCheckArg3;
        }

        const auto vTypeString = GetPGContextTypeString(aContext.IsTiggerPG, vTypeOfCorruption);

        Out(DUMP_FORMAT,
            vPGContext,
            vPGReason,
            aContext.IsTiggerPG, vTypeOfCorruption, vTypeString,
            vFailureDependent,
            aContext.PGPageBase,
            aContext.Prcb,
            vPGSelfValidation,
            vRtlLookupFunctionEntryEx,
            vFsRtlUninitializeSmallMcb,
            vFsRtlMdlReadCompleteDevEx,
            vPGProtectCode2Table,
            aContext.ContextSizeInQWord * sizeof(UINT64) + sizeof(wdk::build_7601::PGContext::PGContextHeader),
            aContext.DcpRoutineToBeScheduled,
            aContext.WorkerRoutine,
            aContext.NumberOfProtectCodes,
            aContext.NumberOfProtectValues,
            vProtectCodes,
            vProtectValues);

        return S_OK;
    }

    template<>
    auto PGKd::DumpPatchGuardContext(
        UINT64 aPGContext,
        UINT64 aPGReason,
        UINT64 aFailureDependent,
        UINT64 aTypeOfCorruption,
        wdk::build_9200::PGContext & aContext)
        -> HRESULT
    {
        static const char DUMP_FORMAT[] = R"RAW(
    PatchGuard Context      : %y, An address of PatchGuard context
    Validation Data         : %y, An address of validation data that caused the error
    Type of Corruption      : Available %I64d   : %I64x : %s
    Failure type dependent information      : %y
    Allocated memory base                   : %y
    Prcb                                    : %y 
    PGSelfValidation                        : %y
    RtlLookupFunctionEntryEx                : %y
    FsRtlUninitializeSmallMcb               : %y
    FsRtlMdlReadCompleteDevEx               : %y
    ContextSizeInBytes                      : %08x (Can be broken)
    DPC Routine                             : %y
    WorkerRoutine                           : %y
    Number Of Protected Codes               : %08x
    Number Of Protected Values              : %08x
    Protected Codes   Table                 : %y
    Protected Values  Table                 : %y
    Protected Strings Table                 : %y

)RAW";

        auto FsRtlUninitializeSmallMcb = 0I64;
        auto hr = m_Symbols->GetOffsetByName("nt!FsRtlUninitializeSmallMcb", (PUINT64)&FsRtlUninitializeSmallMcb);
        if (FAILED(hr))
        {
            return hr;
        }
        auto FsRtlMdlReadCompleteDevEx = 0I64;
        hr = m_Symbols->GetOffsetByName("nt!FsRtlMdlReadCompleteDevEx", (PUINT64)&FsRtlMdlReadCompleteDevEx);
        if (FAILED(hr))
        {
            return hr;
        }
        auto vFsRtlXXXOffset = FsRtlUninitializeSmallMcb - FsRtlMdlReadCompleteDevEx;

        auto vPGContext     = aPGContext;
        auto vPGReason      = aPGReason;
        auto vFailureDependent = aFailureDependent;
        auto vTypeOfCorruption = aTypeOfCorruption;
        auto vPGSelfValidation = aPGContext + aContext.OffsetOfPGSelfValidation;
        auto vFsRtlUninitializeSmallMcb = aPGContext + aContext.OffsetOfFsRtlUninitializeSmallMcb;
        auto vFsRtlMdlReadCompleteDevEx = vFsRtlUninitializeSmallMcb - vFsRtlXXXOffset;
        auto vRtlLookupFunctionEntryEx  = aPGContext + aContext.OffsetOfRtlLookupFunctionEntryEx;
        auto vProtectCodes  = aPGContext    + sizeof(aContext);
        auto vProtectValues = vProtectCodes + (sizeof(wdk::PGProtectCode) * aContext.NumberOfProtectCodes);
        auto vProtectStrings= aPGContext    + offsetof(wdk::build_9200::PGContext, PGProtectStrings.Strings);

        if (aContext.IsTiggerPG)
        {
            vPGContext  = aContext.BugCheckArg0 - BUGCHECK_109_ARGS0_KEY;
            vPGReason   = aContext.BugCheckArg1 - BUGCHECK_109_ARGS1_KEY;
            vFailureDependent = aContext.BugCheckArg2;
            vTypeOfCorruption = aContext.BugCheckArg3;
        }

        const auto vTypeString = GetPGContextTypeString(aContext.IsTiggerPG, vTypeOfCorruption);

        Out(DUMP_FORMAT,
            vPGContext,
            vPGReason,
            aContext.IsTiggerPG, vTypeOfCorruption, vTypeString,
            vFailureDependent,
            aContext.PGPageBase,
            aContext.Prcb,
            vPGSelfValidation,
            vRtlLookupFunctionEntryEx,
            vFsRtlUninitializeSmallMcb,
            vFsRtlMdlReadCompleteDevEx,
            aContext.ContextSizeInQWord * sizeof(UINT64) + sizeof(wdk::build_9200::PGContextHeader),
            aContext.DcpRoutineToBeScheduled,
            aContext.WorkerRoutine,
            aContext.NumberOfProtectCodes,
            aContext.NumberOfProtectValues,
            vProtectCodes,
            vProtectValues,
            vProtectStrings);

        return S_OK;
    }

    template<typename T>
    auto PGKd::DumpPatchGuardContext(
        UINT64 aPGContext,
        UINT64 aPGReason,
        UINT64 aFailureDependent,
        UINT64 aTypeOfCorruption,
        T& aContext)
        -> HRESULT
    {
        static const char DUMP_FORMAT[] = R"RAW(
    PatchGuard Context      : %y, An address of PatchGuard context
    Validation Data         : %y, An address of validation data that caused the error
    Type of Corruption      : Available %I64d   : %I64x : %s
    Failure type dependent information      : %y
    Allocated memory base                   : %y
    Prcb                                    : %y 
    PGSelfValidation                        : %y
    RtlLookupFunctionEntryEx                : %y
    FsRtlUninitializeSmallMcb               : %y
    FsRtlMdlReadCompleteDevEx               : %y
    FsRtlUnknown0                           : %y
    FsRtlUnknown1                           : %y
    ContextSizeInBytes                      : %08x (Can be broken)
    DPC Routine                             : %y
    WorkerRoutine                           : %y
    Number Of Protected Codes               : %08x
    Number Of Protected Values              : %08x
    Protected Codes   Table                 : %y
    Protected Values  Table                 : %y
    Protected Strings Table                 : %y

)RAW";

        auto FsRtlUninitializeSmallMcb = 0I64;
        auto hr = m_Symbols->GetOffsetByName("nt!FsRtlUninitializeSmallMcb", (PUINT64)&FsRtlUninitializeSmallMcb);
        if (FAILED(hr))
        {
            return hr;
        }
        auto FsRtlMdlReadCompleteDevEx = 0I64;
        hr = m_Symbols->GetOffsetByName("nt!FsRtlMdlReadCompleteDevEx", (PUINT64)&FsRtlMdlReadCompleteDevEx);
        if (FAILED(hr))
        {
            return hr;
        }
        auto vFsRtlXXXOffset = FsRtlUninitializeSmallMcb - FsRtlMdlReadCompleteDevEx;

        auto vPGContext = aPGContext;
        auto vPGReason  = aPGReason;
        auto vFailureDependent = aFailureDependent;
        auto vTypeOfCorruption = aTypeOfCorruption;
        auto vPGSelfValidation = aPGContext + aContext.OffsetOfPGSelfValidation;
        auto vRtlLookupFunctionEntryEx  = aPGContext + aContext.OffsetOfRtlLookupFunctionEntryEx;
        auto vFsRtlUninitializeSmallMcb = aPGContext + aContext.OffsetOfFsRtlUninitializeSmallMcb;
        auto vFsRtlMdlReadCompleteDevEx = vFsRtlUninitializeSmallMcb - vFsRtlXXXOffset;
        auto vFsRtlUnkonwn0 = aContext.OffsetOfFsRtlUnknown0 ? aPGContext + aContext.OffsetOfFsRtlUnknown0 : 0;
        auto vFsRtlUnkonwn1 = aContext.OffsetOfFsRtlUnkonwn1 ? aPGContext + aContext.OffsetOfFsRtlUnkonwn1 : 0;
        auto vProtectCodes      = aPGContext    + sizeof(aContext);
        auto vProtectValues     = vProtectCodes + (sizeof(wdk::PGProtectCode) * aContext.NumberOfProtectCodes);
        auto vProtectStrings    = aPGContext    + offsetof(T, PGProtectStrings.Strings);

        if (aContext.IsTiggerPG)
        {
            vPGContext  = aContext.BugCheckArg0 - BUGCHECK_109_ARGS0_KEY;
            vPGReason   = aContext.BugCheckArg1 - BUGCHECK_109_ARGS1_KEY;
            vFailureDependent = aContext.BugCheckArg2;
            vTypeOfCorruption = aContext.BugCheckArg3;
        }

        const auto vTypeString = GetPGContextTypeString(aContext.IsTiggerPG, vTypeOfCorruption);

        Out(DUMP_FORMAT,
            vPGContext,
            vPGReason,
            aContext.IsTiggerPG, vTypeOfCorruption, vTypeString,
            vFailureDependent,
            aContext.PGPageBase,
            aContext.Prcb,
            vPGSelfValidation,
            vRtlLookupFunctionEntryEx,
            vFsRtlUninitializeSmallMcb,
            vFsRtlMdlReadCompleteDevEx,
            vFsRtlUnkonwn0,
            vFsRtlUnkonwn1,
            aContext.ContextSizeInQWord * sizeof(UINT64) + sizeof(T::PGContextHeader),
            aContext.DcpRoutineToBeScheduled,
            aContext.WorkerRoutine,
            aContext.NumberOfProtectCodes,
            aContext.NumberOfProtectValues,
            vProtectCodes,
            vProtectValues,
            vProtectStrings);

        return S_OK;
    }

    auto PGKd::DumpPatchGuard(
        UINT64 aPGContext,
        UINT64 aPGReason,
        UINT64 aFailureDependent,
        UINT64 aTypeOfCorruption,
        bool aNeedBugCheckBanner)
        -> HRESULT
    {
        // Displayed as a banner if NeedBugCheckBanner is true
        static const char BUG_CHECK_BANNER[] = R"RAW(
*******************************************************************************
*                                                                             *
*                        PatchGuard Bugcheck Analysis                         *
*                                                                             *
*******************************************************************************

CRITICAL_STRUCTURE_CORRUPTION (0x109)
)RAW";

        HRESULT hr = S_OK;

        for (;;)
        {
            if (aNeedBugCheckBanner)
            {
                Out(BUG_CHECK_BANNER);
            }

            auto vPGFuncTableOffset = 0u;

            switch (GetSystemVersion())
            {
            default:
                Err("Unsupported version !\n");
                hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
                break;

            case wdk::SystemVersion::Windows7:
            case wdk::SystemVersion::Windows7_SP1:
                vPGFuncTableOffset = sizeof(wdk::build_7601::PGContextHeader);
                hr = DumpPatchGuardImpl<wdk::build_7601::PGContext>(
                    aPGContext, aPGReason, aFailureDependent, aTypeOfCorruption);
                break;

            case wdk::SystemVersion::Windows8:
                vPGFuncTableOffset = sizeof(wdk::build_9200::PGContextHeader);
                hr = DumpPatchGuardImpl<wdk::build_9200::PGContext>(
                    aPGContext, aPGReason, aFailureDependent, aTypeOfCorruption);
                break;

            case wdk::SystemVersion::Windows10_1507:
                vPGFuncTableOffset = sizeof(wdk::build_10240::PGContextHeader);
                hr = DumpPatchGuardImpl<wdk::build_10240::PGContext>(
                    aPGContext, aPGReason, aFailureDependent, aTypeOfCorruption);
                break;

            case wdk::SystemVersion::Windows10_1511:
                vPGFuncTableOffset = sizeof(wdk::build_10586::PGContextHeader);
                hr = DumpPatchGuardImpl<wdk::build_10586::PGContext>(
                    aPGContext, aPGReason, aFailureDependent, aTypeOfCorruption);
                break;

            case wdk::SystemVersion::Windows10_1607:
                vPGFuncTableOffset = sizeof(wdk::build_14393::PGContextHeader);
                hr = DumpPatchGuardImpl<wdk::build_14393::PGContext>(
                    aPGContext, aPGReason, aFailureDependent, aTypeOfCorruption);
                break;

            case wdk::SystemVersion::Windows10_1703:
                vPGFuncTableOffset = sizeof(wdk::build_15063::PGContextHeader);
                hr = DumpPatchGuardImpl<wdk::build_15063::PGContext>(
                    aPGContext, aPGReason, aFailureDependent, aTypeOfCorruption);
                break;

            case wdk::SystemVersion::Windows10_1709:
                vPGFuncTableOffset = sizeof(wdk::build_16299::PGContextHeader);
                hr = DumpPatchGuardImpl<wdk::build_16299::PGContext>(
                    aPGContext, aPGReason, aFailureDependent, aTypeOfCorruption);
                break;

            case wdk::SystemVersion::Windows10_1803:
                vPGFuncTableOffset = sizeof(wdk::build_17134::PGContextHeader);
                hr = DumpPatchGuardImpl<wdk::build_17134::PGContext>(
                    aPGContext, aPGReason, aFailureDependent, aTypeOfCorruption);
                break;
            }
            if (FAILED(hr))
            {
                break;
            }

            // In the case of type 0x106, the address of PatchGuard context is not
            // given (it does not exist).
            if (!aPGContext)
            {
                Out("\n");
                break;
            }

            // Display extra guide messages to help users to explore more details
            Out("Use:\n");

            auto ss = std::stringstream();
            ss << "dps " << std::setfill('0') << std::setw(16) << std::hex << aPGContext << "+" << vPGFuncTableOffset;
            auto cmd = ss.str();

            Out("    ");
            DmlCmdExec(cmd.c_str(), cmd.c_str());
            Out(" to display the structure of PatchGuard\n");

            Out("\n");
            break;
        }

        return hr;
    }

    auto GetNumberOfDistinctiveNumbers(__in PVOID aAddress, __in SIZE_T aSize)
        -> ULONG
    {
        const auto vCursor = static_cast<UCHAR*>(aAddress);

        ULONG vCount = 0;
        for (SIZE_T i = 0u; i < aSize; ++i)
        {
            if (vCursor[i] == 0xff || vCursor[i] == 0x00)
            {
                vCount++;
            }
        }

        return vCount;
    }

    auto GetRamdomness(__in PVOID aAddress, __in SIZE_T aSize)
        -> ULONG
    {
        const auto vCursor = static_cast<UCHAR*>(aAddress);

        std::set<UCHAR> vDict;
        for (SIZE_T i = 0u; i < aSize; ++i)
        {
            auto vResult = vDict.emplace(vCursor[i]);
        }

        return static_cast<ULONG>(vDict.size());
    }

}
