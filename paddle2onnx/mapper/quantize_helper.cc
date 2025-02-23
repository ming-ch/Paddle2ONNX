// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "paddle2onnx/mapper/quantize_helper.h"

namespace paddle2onnx {

void QuantizeModelProcessor::RemoveNodeByName(
    const std::map<std::string,
                   std::vector<std::shared_ptr<ONNX_NAMESPACE::NodeProto>>>&
        name2node_dict,
    std::vector<std::shared_ptr<ONNX_NAMESPACE::NodeProto>>* nodes,
    const std::string& name) {
  for (auto iter = nodes->begin(); iter != nodes->end(); iter++) {
    if ((*iter)->name() == name) {
      std::string input_name = (*iter)->input(0);
      std::string output_name = (*iter)->output(0);
      nodes->erase(iter);
      ReplaceInputOfAllNodes(name2node_dict, output_name, input_name);
      return;
    }
  }
}

void QuantizeModelProcessor::ReplaceInputOfAllNodes(
    const std::map<std::string,
                   std::vector<std::shared_ptr<ONNX_NAMESPACE::NodeProto>>>&
        name2node_dict,
    const std::string& old_name, const std::string& new_name) {
  auto iter = name2node_dict.find(old_name);
  std::vector<std::shared_ptr<ONNX_NAMESPACE::NodeProto>> need_remove_nodes;
  if (iter != name2node_dict.end()) {
    need_remove_nodes = iter->second;
  }
  for (auto& node : need_remove_nodes) {
    std::vector<std::string> inputs;
    for (size_t i = 0; i < node->input_size(); ++i) {
      if (node->input(i) == old_name) {
        inputs.push_back(new_name);
      } else {
        inputs.push_back(node->input(i));
      }
    }
    node->clear_input();
    for (auto in : inputs) {
      node->add_input(in);
    }
  }
}

void QuantizeModelProcessor::InputNameToNodes(
    const std::vector<std::shared_ptr<ONNX_NAMESPACE::NodeProto>>& nodes,
    std::map<std::string,
             std::vector<std::shared_ptr<ONNX_NAMESPACE::NodeProto>>>*
        name2node_dict) {
  for (auto& node : nodes) {
    for (size_t i = 0; i < node->input_size(); ++i) {
      std::string node_input = node->input(i);
      if (name2node_dict->find(node_input) != name2node_dict->end()) {
        (*name2node_dict)[node_input].push_back(node);
      } else {
        std::vector<std::shared_ptr<ONNX_NAMESPACE::NodeProto>> next_nodes;
        (*name2node_dict)[node_input] = next_nodes;
        (*name2node_dict)[node_input].push_back(node);
      }
    }
  }
}

void QuantizeModelProcessor::ProcessQuantizeModel(
    std::vector<std::shared_ptr<ONNX_NAMESPACE::NodeProto>>* parameters,
    std::vector<std::shared_ptr<ONNX_NAMESPACE::ValueInfoProto>>* inputs,
    std::vector<std::shared_ptr<ONNX_NAMESPACE::ValueInfoProto>>* outputs,
    std::vector<std::shared_ptr<ONNX_NAMESPACE::NodeProto>>* nodes,
    OnnxHelper& helper, const std::string& deploy_backend) {
  // Determine whether the model contains quantization-related OPs, if not, exit
  // directly
  bool quantized_model = false;
  for (auto& node : *nodes) {
    if (node->op_type() == "QuantizeLinear" ||
        node->op_type() == "DequantizeLinear") {
      quantized_model = true;
      break;
    }
  }
  if (!quantized_model) {
    return;
  }
#ifdef PADDLE2ONNX_DEBUG
  P2OLogger(true)
      << "Converting model is a quantized model, and your deploy_backend is: "
      << deploy_backend << std::endl;
#endif
  // Determine the format of the exported ONNX quantization model according to
  // the deploy_backend
  if (deploy_backend == "others") {
    // If deploy_backend is others, the quantization model is exported as a
    // float model + quantization table.
    RemoveAllQuantizeOps(parameters, inputs, outputs, nodes, helper);
    std::ofstream outfile;
    outfile.open("max_range.txt", std::ios::out);
    if (!outfile.is_open()) {
      P2OLogger() << "[WARNING] Quantize model processer failed to write range "
                     "information in current location."
                  << std::endl;
      return;
    }
    for (auto iter = helper.quantize_info.begin();
         iter != helper.quantize_info.end(); iter++) {
      std::string log = iter->first;
      auto scale = iter->second.scale_;
      if (scale.size() == 1) {
        log = log + ": " + std::to_string(scale[0] * 127);
        outfile << log << std::endl;
      }
    }
    outfile.close();
  }
}

void QuantizeModelProcessor::RemoveAllQuantizeOps(
    std::vector<std::shared_ptr<ONNX_NAMESPACE::NodeProto>>* parameters,
    std::vector<std::shared_ptr<ONNX_NAMESPACE::ValueInfoProto>>* inputs,
    std::vector<std::shared_ptr<ONNX_NAMESPACE::ValueInfoProto>>* outputs,
    std::vector<std::shared_ptr<ONNX_NAMESPACE::NodeProto>>* nodes,
    OnnxHelper& helper) {
  int node_num = 0;
  for (auto iter = helper.quantize_info.begin();
       iter != helper.quantize_info.end(); iter++) {
    node_num++;
  }
  std::map<std::string, std::vector<std::shared_ptr<ONNX_NAMESPACE::NodeProto>>>
      name2node_dict;
  InputNameToNodes(*nodes, &name2node_dict);
  for (auto iter = nodes->begin(); iter < nodes->end(); iter++) {
    auto node = *iter;
    if (node->op_type() != "QuantizeLinear") {
      continue;
    }
    auto next_node_names = name2node_dict[node->output(0)];

    if (next_node_names.empty() || !next_node_names[0]->has_op_type() ||
        next_node_names[0]->op_type() != "DequantizeLinear") {
      continue;
    }
    std::string input_name = node->input(0);
    RemoveNodeByName(name2node_dict, nodes, node->name());
    std::string output_name = next_node_names[0]->output(0);
    RemoveNodeByName(name2node_dict, nodes, next_node_names[0]->name());
    ReplaceInputOfAllNodes(name2node_dict, output_name, input_name);
  }
}
}
