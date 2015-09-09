#include <pxp-agent/external_module.hpp>

#define LEATHERMAN_LOGGING_NAMESPACE "puppetlabs.pxp_agent.external_module"
#include <leatherman/logging/logging.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <boost/process.hpp>

#include <atomic>
#include <memory>  // shared_ptr

// TODO(ale): disable assert() once we're confident with the code...
// To disable assert()
// #define NDEBUG
#include <cassert>

namespace PXPAgent {

static const std::string METADATA_SCHEMA_NAME { "external_module_metadata" };
static const std::string ACTION_SCHEMA_NAME { "action_metadata" };

// TODO(ale): move this to cpp_pxp_client lib
static const std::string METADATA_CONFIGURATION_ENTRY { "configuration" };
static const std::string METADATA_ACTIONS_ENTRY { "actions" };

//
// Free functions
//

// TODO(ale): use leatherman's execution
// Execute binaries and get output and errors
void runCommand(const std::string& exec,
                std::vector<std::string> args,
                std::string stdin,
                std::string &stdout,
                std::string &stderr) {
    boost::process::context context;
    context.stdin_behavior = boost::process::capture_stream();
    context.stdout_behavior = boost::process::capture_stream();
    context.stderr_behavior = boost::process::capture_stream();
    context.environment = boost::process::self::get_environment();
    boost::process::child child = boost::process::launch(exec, args, context);

    boost::process::postream &in = child.get_stdin();
    in << stdin;
    in.close();

    boost::process::pistream &out = child.get_stdout();
    std::string line;
    while (std::getline(out, line)) {
        stdout += line;
    }

    boost::process::pistream &err = child.get_stderr();
    while (std::getline(err, line)) {
        stderr += line;
    }

    child.wait();
}

// Provides the module metadata validator
PCPClient::Validator getMetadataValidator() {
    // Metadata schema
    PCPClient::Schema metadata_schema { METADATA_SCHEMA_NAME,
                                          PCPClient::ContentType::Json };
    using T_C = PCPClient::TypeConstraint;
    metadata_schema.addConstraint("description", T_C::String, true);
    metadata_schema.addConstraint(METADATA_CONFIGURATION_ENTRY, T_C::Object, false);
    metadata_schema.addConstraint(METADATA_ACTIONS_ENTRY, T_C::Array, true);

    // 'actions' is an array of actions; define the action sub_schema
    PCPClient::Schema action_schema { ACTION_SCHEMA_NAME,
                                      PCPClient::ContentType::Json };
    action_schema.addConstraint("description", T_C::String, false);
    action_schema.addConstraint("name", T_C::String, true);
    action_schema.addConstraint("input", T_C::Object, true);
    action_schema.addConstraint("output", T_C::Object, true);

    metadata_schema.addConstraint(METADATA_ACTIONS_ENTRY, action_schema, false);

    PCPClient::Validator validator {};
    validator.registerSchema(metadata_schema);
    return validator;
}

//
// Public interface
//

ExternalModule::ExternalModule(const std::string& path)
        : path_ { path },
          config_ { "{}" } {
    boost::filesystem::path module_path { path };
    module_name = module_path.filename().string();
    auto metadata = getMetadata();

    try {
        if (metadata.includes(METADATA_CONFIGURATION_ENTRY)) {
            registerConfiguration(
                metadata.get<lth_jc::JsonContainer>(METADATA_CONFIGURATION_ENTRY));
        } else {
            LOG_DEBUG("Found no configuration schema for module '%1%'", module_name);
        }

        for (auto& action : metadata.get<std::vector<lth_jc::JsonContainer>>(
                                        METADATA_ACTIONS_ENTRY)) {
            registerAction(action);
        }
    } catch (lth_jc::data_error& e) {
        LOG_ERROR("Failed to retrieve metadata of module %1%: %2%",
                  module_name, e.what());
        std::string err { "invalid metadata of module " + module_name };
        throw Module::LoadingError { err };
    }
}

void ExternalModule::validateAndSetConfiguration(const lth_jc::JsonContainer& config) {
    if (config_validator_.includesSchema(module_name)) {
        config_validator_.validate(config, module_name);
        config_ = config;
    } else {
        LOG_DEBUG("The '%1%' configuration will not be validated; no JSON "
                  "schema is available");
    }
}

//
// Private interface
//

// Metadata validator (static member)
const PCPClient::Validator ExternalModule::metadata_validator_ {
        getMetadataValidator() };


// Retrieve and validate the module metadata
const lth_jc::JsonContainer ExternalModule::getMetadata() {
    std::string metadata_txt;
    std::string error;

    runCommand(path_, { path_, "metadata" }, "", metadata_txt, error);

    if (!error.empty()) {
        LOG_ERROR("Failed to load the external module metadata from %1%: %2%",
                  path_, error);
        throw Module::LoadingError { "failed to load external module metadata" };
    }

    lth_jc::JsonContainer metadata { metadata_txt };

    try {
        metadata_validator_.validate(metadata, METADATA_SCHEMA_NAME);
        LOG_INFO("External module %1%: metadata validation OK", module_name);
    } catch (PCPClient::validation_error& e) {
        throw Module::LoadingError { std::string { "metadata validation failure: " }
                                     + e.what() };
    }

    return metadata;
}

void ExternalModule::registerConfiguration(const lth_jc::JsonContainer& config_metadata) {
    try {
        PCPClient::Schema configuration_schema { module_name, config_metadata };
        LOG_DEBUG("Registering module config schema for '%1%'", module_name);
        config_validator_.registerSchema(configuration_schema);
    } catch (PCPClient::schema_error& e) {
        LOG_ERROR("Failed to parse the configuration schema of module '%1%': %2%",
                  module_name, e.what());
        std::string err { "invalid configuration schema of module " + module_name };
        throw Module::LoadingError { err };
    }
}

// Register the specified action after ensuring that the input and
// output schemas are valid JSON (i.e. we can instantiate Schema).
void ExternalModule::registerAction(const lth_jc::JsonContainer& action) {
    // TODO(ale): use '<module>_<action>' instead of just 'action' as
    // the schema name, to allow the same action name in different
    // modules, otherwise Validator::registerSchema() will error

    // NOTE(ale): name, input, and output are required action entries
    auto action_name = action.get<std::string>("name");
    LOG_INFO("Validating action '%1% %2%'", module_name, action_name);

    try {
        auto input_schema_json = action.get<lth_jc::JsonContainer>("input");
        PCPClient::Schema input_schema { action_name, input_schema_json };

        auto output_schema_json = action.get<lth_jc::JsonContainer>("output");
        PCPClient::Schema output_schema { action_name, output_schema_json };

        // Metadata schemas are valid JSON; store metadata
        LOG_INFO("Action '%1% %2%' has been validated", module_name, action_name);
        actions.push_back(action_name);
        input_validator_.registerSchema(input_schema);
        output_validator_.registerSchema(output_schema);
    } catch (PCPClient::schema_error& e) {
        LOG_ERROR("Failed to parse metadata schemas of action '%1% %2%': %3%",
                  module_name, action_name, e.what());
        std::string err { "invalid schemas of '" + module_name + " "
                          + action_name + "'" };
        throw Module::LoadingError { err };
    } catch (lth_jc::data_error& e) {
        LOG_ERROR("Failed to retrieve metadata schemas of action '%1% %2%': %3%",
                  module_name, action_name, e.what());
        std::string err { "invalid metadata of '" + module_name + " "
                          + action_name + "'" };
        throw Module::LoadingError { err };
    }
}


ActionOutcome ExternalModule::callAction(const ActionRequest& request) {
    std::string stdout {};
    std::string stderr {};
    auto& action_name = request.action();

    lth_jc::JsonContainer request_input {};
    request_input.set<lth_jc::JsonContainer>("params", request.params());
    request_input.set<lth_jc::JsonContainer>("config", config_);
    auto request_input_txt = request_input.toString();

    LOG_INFO("About to execute '%1% %2%' - request input: %3%",
             module_name, action_name, request_input_txt);

    runCommand(path_, { path_, action_name }, request_input_txt, stdout, stderr);

    if (stdout.empty()) {
        LOG_DEBUG("'%1% %2%' produced no output", module_name, action_name);
    } else {
        LOG_DEBUG("'%1% %2%' output: %3%", module_name, action_name, stdout);
    }

    if (!stderr.empty()) {
        LOG_ERROR("'%1% %2%' error: %3%", module_name, action_name, stderr);
    }

    // Ensure output format is valid JSON by instantiating JsonContainer
    lth_jc::JsonContainer results {};

    try {
        results = lth_jc::JsonContainer { stdout };
    } catch (lth_jc::data_parse_error& e) {
        LOG_ERROR("'%1% %2%' output is not valid JSON: %3%",
                  module_name, action_name, e.what());
        std::string err_msg { "'" + module_name + " " + action_name + "' "
                              "returned invalid JSON - stderr: " + stderr };
        throw Module::ProcessingError { err_msg };
    }

    return ActionOutcome { stderr, stdout, results };
}

}  // namespace PXPAgent
