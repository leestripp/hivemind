#pragma once

#include "BaseTool.h"
#include "core/SecondBrain.h"

class BrainTool : public BaseTool {
public:
    BrainTool();

    std::string get_name() const override { return "brain"; }
    std::string get_description() const override { return description_; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;

    static json do_capture(hive::SecondBrain& brain, const json& args);
    static json do_search(hive::SecondBrain& brain, const json& args);
    static json do_read(hive::SecondBrain& brain, const json& args);
    static json do_list(hive::SecondBrain& brain, const json& args);
    static json do_update(hive::SecondBrain& brain, const json& args);
    static json do_append(hive::SecondBrain& brain, const json& args);
    static json do_file(hive::SecondBrain& brain, const json& args);
    static json do_backlinks(hive::SecondBrain& brain, const json& args);
    static json do_delete(hive::SecondBrain& brain, const json& args);
    static json do_reindex(hive::SecondBrain& brain);

private:
    static json meta_json(const hive::BrainNoteMeta& meta);
    std::string description_;
};
