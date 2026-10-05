#include "HiveVoiceApp.h"

int main(int argc, char** argv) {
    auto app = hv::HiveVoiceApp::create();
    return app->run(argc, argv);
}
