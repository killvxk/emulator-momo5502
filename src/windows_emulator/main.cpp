#include "std_include.hpp"

#include "emulator_utils.hpp"
#include "process_context.hpp"
#include "syscalls.hpp"

#include "reflect_extension.hpp"
#include <reflect>

#include <unicorn_x64_emulator.hpp>

#define GS_SEGMENT_ADDR 0x6000000ULL
#define GS_SEGMENT_SIZE (20 << 20)  // 20 MB

#define IA32_GS_BASE_MSR 0xC0000101

#define STACK_SIZE 0x40000
#define STACK_ADDRESS (0x800000000000 - STACK_SIZE)

#define KUSD_ADDRESS 0x7ffe0000

namespace
{
	void setup_stack(x64_emulator& emu, const uint64_t stack_base, const size_t stack_size)
	{
		emu.allocate_memory(stack_base, stack_size, memory_permission::read_write);

		const uint64_t stack_end = stack_base + stack_size;
		emu.reg(x64_register::rsp, stack_end);
	}

	emulator_allocator setup_gs_segment(x64_emulator& emu, const uint64_t segment_base, const uint64_t size)
	{
		struct msr_value
		{
			uint32_t id;
			uint64_t value;
		};

		const msr_value value{
			IA32_GS_BASE_MSR,
			segment_base
		};

		emu.write_register(x64_register::msr, &value, sizeof(value));
		emu.allocate_memory(segment_base, size, memory_permission::read_write);

		return {emu, segment_base, size};
	}

	emulator_object<KUSER_SHARED_DATA> setup_kusd(x64_emulator& emu)
	{
		emu.allocate_memory(KUSD_ADDRESS, page_align_up(sizeof(KUSER_SHARED_DATA)), memory_permission::read);

		const emulator_object<KUSER_SHARED_DATA> kusd_object{emu, KUSD_ADDRESS};
		kusd_object.access([](KUSER_SHARED_DATA& kusd)
		{
			const auto& real_kusd = *reinterpret_cast<KUSER_SHARED_DATA*>(KUSD_ADDRESS);

			memcpy(&kusd, &real_kusd, sizeof(kusd));

			kusd.ImageNumberLow = IMAGE_FILE_MACHINE_I386;
			kusd.ImageNumberHigh = IMAGE_FILE_MACHINE_AMD64;

			memset(&kusd.ProcessorFeatures, 0, sizeof(kusd.ProcessorFeatures));

			// ...
		});

		return kusd_object;
	}

	mapped_binary map_module(x64_emulator& emu, const std::vector<uint8_t>& module_data,
	                         const std::string& name)
	{
		mapped_binary binary{};

		// TODO: Range checks
		auto* ptr = module_data.data();
		auto* dos_header = reinterpret_cast<const IMAGE_DOS_HEADER*>(ptr);
		auto* nt_headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(ptr + dos_header->e_lfanew);
		auto& optional_header = nt_headers->OptionalHeader;

		binary.image_base = optional_header.ImageBase;
		binary.size_of_image = optional_header.SizeOfImage;

		if (!emu.allocate_memory(binary.image_base, binary.size_of_image, memory_permission::read))
		{
			binary.image_base = emu.find_free_allocation_base(binary.size_of_image);
			if ((optional_header.DllCharacteristics &
					IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE) == 0 || //
				!emu.allocate_memory(
					binary.image_base, binary.size_of_image, memory_permission::read))
			{
				throw std::runtime_error("Failed to map binary");
			}
		}

		printf("Mapping %s at %llX\n", name.c_str(), binary.image_base);

		emu.write_memory(binary.image_base, ptr, optional_header.SizeOfHeaders);

		const std::span sections(IMAGE_FIRST_SECTION(nt_headers), nt_headers->FileHeader.NumberOfSections);

		for (const auto& section : sections)
		{
			const auto target_ptr = binary.image_base + section.VirtualAddress;

			if (section.SizeOfRawData > 0)
			{
				const void* source_ptr = ptr + section.PointerToRawData;

				const auto size_of_data = std::min(section.SizeOfRawData, section.Misc.VirtualSize);
				emu.write_memory(target_ptr, source_ptr, size_of_data);
			}
			auto permissions = memory_permission::none;

			if (section.Characteristics & IMAGE_SCN_MEM_EXECUTE)
			{
				permissions |= memory_permission::exec;
			}

			if (section.Characteristics & IMAGE_SCN_MEM_READ)
			{
				permissions |= memory_permission::read;
			}

			if (section.Characteristics & IMAGE_SCN_MEM_WRITE)
			{
				permissions |= memory_permission::write;
			}

			const auto size_of_section = page_align_up(std::max(section.SizeOfRawData, section.Misc.VirtualSize));

			emu.protect_memory(target_ptr, size_of_section, permissions, nullptr);
		}

		auto& export_directory_entry = optional_header.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
		if (export_directory_entry.VirtualAddress == 0 || export_directory_entry.Size == 0)
		{
			return binary;
		}

		const auto* export_directory = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(ptr + export_directory_entry.
			VirtualAddress);

		//const auto function_count = export_directory->NumberOfFunctions;
		const auto names_count = export_directory->NumberOfNames;

		const auto* names = reinterpret_cast<const DWORD*>(ptr + export_directory->AddressOfNames);
		const auto* ordinals = reinterpret_cast<const WORD*>(ptr + export_directory->AddressOfNameOrdinals);
		const auto* functions = reinterpret_cast<const DWORD*>(ptr + export_directory->AddressOfFunctions);

		for (DWORD i = 0; i < names_count; i++)
		{
			const auto* function_name = reinterpret_cast<const char*>(ptr + names[i]);
			const auto function_rva = functions[ordinals[i]];
			const auto function_address = binary.image_base + function_rva;

			binary.exports[function_name] = function_address;
		}

		return binary;
	}

	process_context setup_context(x64_emulator& emu)
	{
		setup_stack(emu, STACK_ADDRESS, STACK_SIZE);
		process_context context{};

		context.kusd = setup_kusd(emu);

		context.gs_segment = setup_gs_segment(emu, GS_SEGMENT_ADDR, GS_SEGMENT_SIZE);

		auto& gs = context.gs_segment;

		context.teb = gs.reserve<TEB>();
		context.peb = gs.reserve<PEB>();
		//context.ldr = gs.reserve<PEB_LDR_DATA>();
		context.process_params = gs.reserve<RTL_USER_PROCESS_PARAMETERS>();

		context.teb.access([&](TEB& teb)
		{
			teb.NtTib.StackLimit = reinterpret_cast<void*>(STACK_ADDRESS);
			teb.NtTib.StackBase = reinterpret_cast<void*>((STACK_ADDRESS + STACK_SIZE));
			teb.NtTib.Self = &context.teb.ptr()->NtTib;
			teb.ProcessEnvironmentBlock = context.peb.ptr();
		});

		context.peb.access([&](PEB& peb)
		{
			peb.ImageBaseAddress = nullptr;
			peb.ProcessHeap = nullptr;
			peb.ProcessHeaps = nullptr;
			peb.ProcessParameters = context.process_params.ptr();
		});

		context.process_params.access([&](RTL_USER_PROCESS_PARAMETERS& proc_params)
		{
			proc_params.Flags = 0x6001;
			gs.make_unicode_string(proc_params.ImagePathName, L"C:\\Users\\mauri\\Desktop\\ConsoleApplication6.exe");
			gs.make_unicode_string(proc_params.CommandLine, L"C:\\Users\\mauri\\Desktop\\ConsoleApplication6.exe");
		});

		return context;
	}

	std::vector<uint8_t> load_file(const std::filesystem::path& file)
	{
		std::ifstream stream(file, std::ios::in | std::ios::binary);
		return {(std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>()};
	}

	mapped_binary map_file(x64_emulator& emu, const std::filesystem::path& file)
	{
		const auto data = load_file(file);
		return map_module(emu, data, file.generic_string());
	}

	template <typename T>
	class type_info
	{
	public:
		type_info()
		{
			this->type_name_ = reflect::type_name<T>();

			reflect::for_each<T>([this](auto I)
			{
				const auto member_name = reflect::member_name<I, T>();
				const auto member_offset = reflect::offset_of<I, T>();

				this->members_[member_offset] = member_name;
			});
		}

		std::string get_member_name(const size_t offset) const
		{
			size_t last_offset{};
			std::string_view last_member{};

			for (const auto& member : this->members_)
			{
				if (offset == member.first)
				{
					return member.second;
				}

				if (offset < member.first)
				{
					const auto diff = offset - last_offset;
					return std::string(last_member) + "+" + std::to_string(diff);
				}

				last_offset = member.first;
				last_member = member.second;
			}

			return "<N/A>";
		}

		const std::string& get_type_name() const
		{
			return this->type_name_;
		}

	private:
		std::string type_name_{};
		std::map<size_t, std::string> members_{};
	};


	template <typename T>
	void watch_object(x64_emulator& emu, emulator_object<T> object)
	{
		type_info<T> info{};

		emu.hook_memory_read(object.value(), object.size(),
		                     [i = std::move(info), object](const uint64_t address, size_t)
		                     {
			                     const auto offset = address - object.value();
			                     printf("%s: %llX (%s)\n", i.get_type_name().c_str(), offset,
			                            i.get_member_name(offset).c_str());
		                     });
	}

	void run()
	{
		const auto emu = unicorn::create_x64_emulator();

		auto context = setup_context(*emu);

		context.executable = map_file(*emu, R"(C:\Users\mauri\Desktop\ConsoleApplication6.exe)");

		context.peb.access([&](PEB& peb)
		{
			peb.ImageBaseAddress = reinterpret_cast<void*>(context.executable.image_base);
		});

		context.ntdll = map_file(*emu, R"(C:\Windows\System32\ntdll.dll)");

		const auto entry1 = context.ntdll.exports.at("LdrInitializeThunk");
		const auto entry2 = context.ntdll.exports.at("RtlUserThreadStart");

		(void)entry1;
		(void)entry2;

		std::unordered_map<uint64_t, std::string> export_remap{};
		for (const auto& exp : context.ntdll.exports)
		{
			export_remap.try_emplace(exp.second, exp.first);
		}

		for (const auto& exp : export_remap)
		{
			auto name = exp.second;
			emu->hook_memory_execution(exp.first, 0,
			                           [&emu, n = std::move(name)](const uint64_t address, const size_t)
			                           {
				                           printf("Executing function: %s (%llX)\n", n.c_str(), address);

				                           if (n == "RtlImageNtHeaderEx")
				                           {
					                           printf("Base: %llX\n", emu->reg(x64_register::rdx));
				                           }
			                           });
		}

		emu->hook_instruction(x64_hookable_instructions::syscall, [&]
		{
			handle_syscall(*emu, context);
		});

		watch_object(*emu, context.teb);
		watch_object(*emu, context.peb);
		watch_object(*emu, context.process_params);
		watch_object(*emu, context.kusd);

		emu->hook_memory_execution(0, std::numeric_limits<size_t>::max(), [&](const uint64_t address, const size_t)
		{
			static bool hit = false;
			// if (address == 0x1800D3C80)
			if (address == 0x1800D4420)
			{
				//hit = true;
				//uc.stop();
			}

			//if (hit)
			{
				printf(
					"Inst: %16llX - RAX: %16llX - RBX: %16llX - RCX: %16llX - RDX: %16llX - R8: %16llX - R9: %16llX - RDI: %16llX - RSI: %16llX\n",
					address,
					emu->reg(x64_register::rax), emu->reg(x64_register::rbx), emu->reg(x64_register::rcx),
					emu->reg(x64_register::rdx), emu->reg(x64_register::r8), emu->reg(x64_register::r9),
					emu->reg(x64_register::rdi), emu->reg(x64_register::rsi));
			}
		});

		const auto execution_context = context.gs_segment.reserve<CONTEXT>();

		emu->reg(x64_register::rcx, execution_context.value());
		emu->reg(x64_register::rdx, context.ntdll.image_base);

		try
		{
			emu->start(entry1);
		}
		catch (...)
		{
			printf("Emulation failed at: %llX\n", emu->reg(x64_register::rip));
			throw;
		}

		printf("Emulation done.\n");
	}
}

int main(int /*argc*/, char** /*argv*/)
{
	try
	{
		run();
		return 0;
	}
	catch (std::exception& uce)
	{
		puts(uce.what());

#ifdef _WIN32
		//MessageBoxA(nullptr, e.what(), "ERROR", MB_ICONERROR);
#endif
	}

	return 1;
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, PSTR, int)
{
	return main(__argc, __argv);
}
#endif