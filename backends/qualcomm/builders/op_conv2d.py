# Copyright (c) Qualcomm Innovation Center, Inc.
# All rights reserved
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

from typing import cast, Dict, List

import executorch.backends.qualcomm.python.PyQnnWrapperAdaptor as PyQnnWrapper

import numpy as np
import torch
from executorch.backends.qualcomm.utils.constants import QCOM_DATA

from .node_visitor import NodeVisitor
from .node_visitor_manager import register_node_visitor
from .qnn_constants import (
    OpConv2d,
    OpDepthWiseConv2d,
    OpTransposeConv2d,
    QNN_OP_PACKAGE_NAME_QTI_AISW,
)
from .utils import get_parameter


@register_node_visitor
class Conv2d(NodeVisitor):
    target = ["aten.convolution.default"]

    def __init__(self, *args) -> None:
        super().__init__(*args)

    def _add_conv_op_parameter(
        self,
        OP,
        conv_op,
        conv_input_tensors,
        conv_output_tensors,
        stride,
        stride_shape,
        padding,
        padding_shape,
        dilation,
        dilation_shape,
        output_padding=None,
        output_padding_shape=None,
        transpose_conv=False,
        groups=None,
    ) -> PyQnnWrapper.PyQnnOpWrapper:
        """
        This function is shared among Conv1D, Conv2D, and DepthWise Conv2D as most of the required parameters overlaps.
        """
        conv_op.AddInputTensors(conv_input_tensors)
        conv_op.AddOutputTensors(conv_output_tensors)
        conv_op.AddTensorParam(
            OP.param_stride,
            PyQnnWrapper.Qnn_DataType_t.QNN_DATATYPE_UINT_32,
            len(stride_shape),
            stride_shape,
            np.array(stride, dtype=np.uint32),
            True,
        )
        conv_op.AddTensorParam(
            OP.param_pad_amount,
            PyQnnWrapper.Qnn_DataType_t.QNN_DATATYPE_UINT_32,
            len(padding_shape),
            padding_shape,
            np.array(
                [[padding[0], padding[0]], [padding[1], padding[1]]],
                dtype=np.uint32,
            ),
            True,
        )

        if transpose_conv:
            conv_op.AddTensorParam(
                OP.param_output_padding,
                PyQnnWrapper.Qnn_DataType_t.QNN_DATATYPE_UINT_32,
                len(output_padding_shape),
                output_padding_shape,
                np.array(output_padding, dtype=np.uint32),
                True,
            )
        else:
            conv_op.AddTensorParam(
                OP.param_dilation,
                PyQnnWrapper.Qnn_DataType_t.QNN_DATATYPE_UINT_32,
                len(dilation_shape),
                dilation_shape,
                np.array(dilation, dtype=np.uint32),
                True,
            )

        if groups is not None:
            conv_op.AddScalarParam(
                OP.param_group,
                PyQnnWrapper.Qnn_DataType_t.QNN_DATATYPE_UINT_32,
                {QCOM_DATA: np.uint32(groups)},
            )

        return conv_op

    def define_node(
        self,
        node: torch.fx.Node,
        nodes_to_wrappers: Dict[str, PyQnnWrapper.TensorWrapper],
    ) -> PyQnnWrapper.PyQnnOpWrapper:
        input_node = self.get_node(node.args[0])
        input_tensor = self.get_tensor(input_node, node)
        assert (
            input_tensor.dim() == 4
        ), "All Conv should be converted to Conv2D in ConvertConv1dToConv2d"
        input_tensor_wrapper = self.define_tensor(
            input_node,
            node,
            input_tensor,
            PyQnnWrapper.Qnn_TensorType_t.QNN_TENSOR_TYPE_NATIVE,
            nodes_to_wrappers,
        )

        filter_node = self.get_node(node.args[1])
        filter_tensor = get_parameter(filter_node, self.edge_program)
        # weight of pytorch OIHW(conv2d) | IOHW(conv_transpose2d), yet QNN is HWIO
        is_transpose_conv = cast(bool, node.args[6])
        filter_axis_order = (2, 3, 0, 1) if is_transpose_conv else (2, 3, 1, 0)
        filter_tensor = filter_tensor.permute(dims=filter_axis_order).contiguous()
        filter_tensor_wrapper = self.define_tensor(
            filter_node,
            node,
            filter_tensor,
            PyQnnWrapper.Qnn_TensorType_t.QNN_TENSOR_TYPE_STATIC,
            nodes_to_wrappers,
        )
        conv_input_tensors = [input_tensor_wrapper, filter_tensor_wrapper]

        if node.args[2] is not None:
            bias_node = self.get_node(node.args[2])
            bias_tensor = get_parameter(bias_node, self.edge_program)
            bias_tensor_wrapper = self.define_tensor(
                bias_node,
                node,
                bias_tensor,
                PyQnnWrapper.Qnn_TensorType_t.QNN_TENSOR_TYPE_STATIC,
                nodes_to_wrappers,
            )
            conv_input_tensors.append(bias_tensor_wrapper)

        output_tensor = self.get_tensor(node, node)
        output_tensor_wrapper = self.define_tensor(
            node,
            node,
            output_tensor,
            PyQnnWrapper.Qnn_TensorType_t.QNN_TENSOR_TYPE_NATIVE,
            nodes_to_wrappers,
        )
        conv_output_tensors = [output_tensor_wrapper]

        stride = cast(List[int], node.args[3])
        padding = cast(List[int], node.args[4])
        dilation = cast(List[int], node.args[5])
        output_padding = cast(List[int], node.args[7])

        groups = cast(int, node.args[8])
        # Qnn filter tensor is (H, W, Cin, Cout)
        group_input_channels = filter_tensor.shape[2]
        group_output_channels = int(filter_tensor.shape[3] / groups)
        # 1) groups = input_channels (i.e. group_input_channels = 1)
        # 2) output_channels is a positive integer multiple of input channels
        # TODO: Currently, negative results will be zero with Depthwise conv2d when input_channel == groups == 1
        # and test on QNN 2.14 rc1. Need to carefully investigate.
        is_depthwise_conv = (
            (group_input_channels == 1)
            and (group_output_channels % group_input_channels == 0)
            and (groups > 2)
        )
        if len(padding) == 1:
            padding = padding + padding

        stride_shape = [len(stride)]
        padding_shape = [2, 2]
        dilation_shape = [len(dilation)]
        output_padding_shape = [len(output_padding)]

        if is_depthwise_conv:
            op_class = OpDepthWiseConv2d
        elif is_transpose_conv:
            op_class = OpTransposeConv2d
        else:
            op_class = OpConv2d

        conv_op = PyQnnWrapper.PyQnnOpWrapper(
            node.name,
            QNN_OP_PACKAGE_NAME_QTI_AISW,
            op_class.op_name,
        )
        conv_op = self._add_conv_op_parameter(
            op_class,
            conv_op,
            conv_input_tensors,
            conv_output_tensors,
            stride,
            stride_shape,
            padding,
            padding_shape,
            dilation,
            dilation_shape,
            output_padding,
            output_padding_shape,
            is_transpose_conv,
            None if is_depthwise_conv else groups,
        )

        return conv_op
