// hive-client — HiveMind 2.0 native Linux client entry point (GTK4/gtkmm4).
#include "HiveApplication.h"

int main(int argc, char* argv[]) {
    auto app = hive::HiveApplication::create();
    return app->run(argc, argv);
}
