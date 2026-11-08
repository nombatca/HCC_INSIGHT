#pragma once
namespace HCCBackupSdSelfTest {
enum class Result {idle,running,passed,busy,storage,begin,write,commit,verify,abort_cleanup,task};
// Starts a short worker-task test on a new HCC capture directory. No device I/O.
bool request();
bool clear();
Result result();
const char* statusText();
}
