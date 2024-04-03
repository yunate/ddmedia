#include "ddmedia_test/stdafx.h"
#include "ddbase/ddtest_case_factory.h"
#include "ddbase/ddassert.h"
#include "ddbase/windows/ddmoudle_utils.h"
#include "ddbase/ddlocale.h"
#include <process.h>

#pragma comment(lib, "ddbase.lib")
#pragma comment(lib, "ddmedia.lib")
#pragma comment(lib, "ddimage.lib")
namespace NSP_DD {
int test_main()
{
    DDTCF.insert_white_type("test_case_expend_mp4_audio");
    DDTCF.run();
    return 0;
}

} // namespace NSP_DD

void main()
{
    NSP_DD::ddmoudle_utils::set_current_directory(L"");
    NSP_DD::ddlocale::set_utf8_locale_and_io_codepage();
    // ::_CrtSetBreakAlloc(1932);
    int result = NSP_DD::test_main();

#ifdef _DEBUG
    _cexit();
    DDASSERT_FMT(!::_CrtDumpMemoryLeaks(), L"Memory leak!!! Check the output to find the log.");
    ::system("pause");
    ::_exit(result);
#else
    ::system("pause");
    ::exit(result);
#endif
}

