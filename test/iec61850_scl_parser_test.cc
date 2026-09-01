#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "IEC61850SclParser.h"

namespace {

constexpr auto kCompleteScd = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<SCL xmlns="http://www.iec.ch/61850/2003/SCL" version="2007" revision="B">
  <Communication>
    <SubNetwork name="StationBus" type="8-MMS">
      <ConnectedAP iedName="IED1" apName="AP1">
        <Address>
          <P type="IP">192.168.10.20</P>
          <P type="IP-SUBNET">255.255.255.0</P>
          <P type="IP-GATEWAY">192.168.10.1</P>
        </Address>
        <GSE ldInst="LD0" cbName="gcb1">
          <Address>
            <P type="MAC-Address">01-0C-CD-01-00-01</P>
            <P type="APPID">1001</P>
            <P type="VLAN-ID">001</P>
            <P type="VLAN-PRIORITY">4</P>
          </Address>
        </GSE>
        <SMV ldInst="LD0" cbName="smv1">
          <Address>
            <P type="MAC-Address">01-0C-CD-04-00-01</P>
            <P type="APPID">4001</P>
            <P type="VLAN-ID">002</P>
            <P type="VLAN-PRIORITY">4</P>
          </Address>
        </SMV>
      </ConnectedAP>
    </SubNetwork>
  </Communication>
  <IED name="IED1" manufacturer="Megsky" type="Protection" desc="测试IED">
    <AccessPoint name="AP1">
      <Server>
        <LDevice inst="LD0">
          <LN0 lnClass="LLN0" inst="" lnType="ln0">
            <DataSet name="ds1">
              <FCDA ldInst="LD0" lnClass="MMXU" lnInst="1" doName="TotW" daName="mag.f" fc="MX"/>
            </DataSet>
            <ReportControl name="brcb1" rptID="IED1/Measurements" datSet="ds1" buffered="true" confRev="3" intgPd="5000" bufTime="20">
              <TrgOps dchg="true" qchg="true" dupd="false" period="true" gi="true"/>
              <OptFields seqNum="true" timeStamp="true" reasonCode="true" dataSet="true" dataRef="true" bufOvfl="true" entryID="true" configRef="true" segmentation="true"/>
              <RptEnabled max="2"/>
            </ReportControl>
            <GSEControl name="gcb1" appID="Trip" datSet="ds1" confRev="4" fixedOffs="false"/>
            <SampledValueControl name="smv1" smvID="MU01" datSet="ds1" confRev="5" smpRate="80" nofASDU="1" multicast="true"/>
          </LN0>
          <LN prefix="" lnClass="MMXU" inst="1" lnType="ln_mmxu"/>
        </LDevice>
      </Server>
    </AccessPoint>
  </IED>
  <DataTypeTemplates>
    <LNodeType id="ln0" lnClass="LLN0"/>
    <LNodeType id="ln_mmxu" lnClass="MMXU">
      <DO name="TotW" type="do_mv"/>
    </LNodeType>
    <DOType id="do_mv" cdc="MV">
      <DA name="mag" bType="Struct" type="da_vector" fc="MX"/>
      <DA name="q" bType="Quality" fc="MX"/>
      <DA name="t" bType="Timestamp" fc="MX"/>
    </DOType>
    <DAType id="da_vector">
      <BDA name="f" bType="FLOAT32"/>
    </DAType>
  </DataTypeTemplates>
</SCL>)xml";

// 验证：默认命名空间SCD会同时解析MMS、GOOSE、SMV通信地址及控制块。
TEST(IEC61850SclParserTest, ParsesMmsGooseSmvAndControlBlocks) {
  IEC61850::SclParser parser;
  IEC61850Proto::NormalizedSclModel model;
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = parser.Parse("station-model", "station.scd", kCompleteScd, &model, &issues);

  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(issues.empty());
  EXPECT_EQ(model.model_name(), "station-model");
  EXPECT_EQ(model.document_kind(), IEC61850Proto::SCL_DOCUMENT_KIND_SCD);
  ASSERT_EQ(model.ieds_size(), 1);
  EXPECT_EQ(model.ieds(0).name(), "IED1");
  ASSERT_EQ(model.connected_access_points_size(), 1);
  const auto& connected = model.connected_access_points(0);
  EXPECT_EQ(connected.subnetwork_name(), "StationBus");
  EXPECT_EQ(connected.network_type(), "8-MMS");
  const auto summary = IEC61850::SclParser::BuildSummary(model);
  ASSERT_EQ(summary.connected_access_points_size(), 1);
  EXPECT_EQ(summary.connected_access_points(0).ied_name(), "IED1");
  EXPECT_EQ(summary.connected_access_points(0).ap_name(), "AP1");
  EXPECT_EQ(summary.connected_access_points(0).subnetwork_name(), "StationBus");
  EXPECT_EQ(summary.connected_access_points(0).network_type(), "8-MMS");
  ASSERT_EQ(connected.address_size(), 3);
  ASSERT_EQ(connected.gse_size(), 1);
  EXPECT_EQ(connected.gse(0).mac_address(), "01-0C-CD-01-00-01");
  ASSERT_TRUE(connected.gse(0).has_app_id());
  EXPECT_EQ(connected.gse(0).app_id(), 0x1001u);
  ASSERT_EQ(connected.smv_size(), 1);
  EXPECT_EQ(connected.smv(0).app_id(), 0x4001u);

  const auto& ied = model.ieds(0);
  ASSERT_EQ(ied.data_sets_size(), 1);
  EXPECT_EQ(ied.data_sets(0).access_point(), "AP1");
  EXPECT_EQ(ied.data_sets(0).data_set_ref(), "IED1LD0/LLN0$ds1");
  ASSERT_EQ(ied.data_sets(0).members_size(), 1);
  EXPECT_EQ(ied.data_sets(0).members(0).data_ref(), "IED1LD0/MMXU1.TotW.mag.f");
  ASSERT_EQ(ied.report_controls_size(), 1);
  EXPECT_EQ(ied.report_controls(0).access_point(), "AP1");
  EXPECT_EQ(ied.report_controls(0).rcb_ref(), "IED1LD0/LLN0$BR$brcb1");
  EXPECT_EQ(ied.report_controls(0).max_instances(), 2u);
  EXPECT_TRUE(ied.report_controls(0).trigger_options().general_interrogation());
  EXPECT_TRUE(ied.report_controls(0).optional_fields().segmentation());
  ASSERT_EQ(ied.gse_controls_size(), 1);
  EXPECT_EQ(ied.gse_controls(0).access_point(), "AP1");
  EXPECT_EQ(ied.gse_controls(0).control_ref(), "IED1LD0/LLN0$GO$gcb1");
  EXPECT_EQ(ied.gse_controls(0).go_id(), "Trip");
  ASSERT_EQ(ied.sampled_value_controls_size(), 1);
  EXPECT_EQ(ied.sampled_value_controls(0).access_point(), "AP1");
  EXPECT_EQ(ied.sampled_value_controls(0).control_ref(), "IED1LD0/LLN0$MS$smv1");
}

// 验证：同一IED和AccessPoint下的多个ConnectedAP网段摘要全部保留，供上位机精确筛选。
TEST(IEC61850SclParserTest, BuildSummaryPreservesMultipleConnectedApNetworks) {
  IEC61850Proto::NormalizedSclModel model;
  model.set_model_name("multi-network");
  model.set_source_name("multi-network.scd");

  auto* neta = model.add_connected_access_points();
  neta->set_ied_name("IED1");
  neta->set_ap_name("AP1");
  neta->set_subnetwork_name("NETA");
  neta->set_network_type("8-MMS");

  auto* netb = model.add_connected_access_points();
  netb->set_ied_name("IED1");
  netb->set_ap_name("AP1");
  netb->set_subnetwork_name("NETB");
  netb->set_network_type("8-MMS");

  const auto summary = IEC61850::SclParser::BuildSummary(model);

  ASSERT_EQ(summary.connected_access_points_size(), 2);
  EXPECT_EQ(summary.connected_access_points(0).ied_name(), "IED1");
  EXPECT_EQ(summary.connected_access_points(0).ap_name(), "AP1");
  EXPECT_EQ(summary.connected_access_points(0).subnetwork_name(), "NETA");
  EXPECT_EQ(summary.connected_access_points(0).network_type(), "8-MMS");
  EXPECT_EQ(summary.connected_access_points(1).ied_name(), "IED1");
  EXPECT_EQ(summary.connected_access_points(1).ap_name(), "AP1");
  EXPECT_EQ(summary.connected_access_points(1).subnetwork_name(), "NETB");
  EXPECT_EQ(summary.connected_access_points(1).network_type(), "8-MMS");
}

// 验证：LNodeType、DOType与DAType会展开为可用于点映射的规范化数据引用。
TEST(IEC61850SclParserTest, ExpandsDataTypeTemplatesIntoStableReferences) {
  IEC61850::SclParser parser;
  IEC61850Proto::NormalizedSclModel model;
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = parser.Parse("station-model", "station.scd", kCompleteScd, &model, &issues);

  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(model.ieds_size(), 1);
  const auto& ied = model.ieds(0);
  ASSERT_EQ(ied.logical_nodes_size(), 2);
  EXPECT_EQ(ied.logical_nodes(0).access_point(), "AP1");
  EXPECT_EQ(ied.logical_nodes(1).access_point(), "AP1");
  ASSERT_EQ(ied.data_objects_size(), 1);
  EXPECT_EQ(ied.data_objects(0).access_point(), "AP1");
  EXPECT_EQ(ied.data_objects(0).data_ref(), "IED1LD0/MMXU1.TotW");
  EXPECT_EQ(ied.data_objects(0).cdc(), "MV");
  ASSERT_EQ(ied.data_attributes_size(), 3);
  EXPECT_EQ(ied.data_attributes(0).access_point(), "AP1");
  EXPECT_EQ(ied.data_attributes(1).access_point(), "AP1");
  EXPECT_EQ(ied.data_attributes(2).access_point(), "AP1");
  EXPECT_EQ(ied.data_attributes(0).data_ref(), "IED1LD0/MMXU1.TotW.mag.f");
  EXPECT_EQ(ied.data_attributes(0).fc(), IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX);
  EXPECT_EQ(ied.data_attributes(0).basic_type(), "FLOAT32");
  EXPECT_EQ(ied.data_attributes(1).data_ref(), "IED1LD0/MMXU1.TotW.q");
  EXPECT_EQ(ied.data_attributes(2).data_ref(), "IED1LD0/MMXU1.TotW.t");
}

// 验证：Inputs/ExtRef会保留发布端对象、控制块定位和内部地址，并生成稳定数据引用。
TEST(IEC61850SclParserTest, ParsesInputsExternalReferences) {
  const std::string xml = R"xml(
<SCL xmlns="http://www.iec.ch/61850/2003/SCL">
  <IED name="Subscriber">
    <AccessPoint name="AP1"><Server><LDevice inst="LD0">
      <LN lnClass="PTRC" inst="1" lnType="ptrc">
        <Inputs>
          <ExtRef intAddr="trip-in" iedName="Publisher" ldInst="PROT"
                  prefix="A" lnClass="PTRC" lnInst="2" doName="Tr"
                  daName="general" fc="ST" serviceType="GOOSE"
                  srcLDInst="LD0" srcPrefix="" srcLNClass="LLN0"
                  srcLNInst="" srcCBName="gcbTrip"/>
        </Inputs>
      </LN>
    </LDevice></Server></AccessPoint>
  </IED>
  <DataTypeTemplates><LNodeType id="ptrc" lnClass="PTRC"/></DataTypeTemplates>
</SCL>)xml";
  IEC61850::SclParser parser;
  IEC61850Proto::NormalizedSclModel model;
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = parser.Parse("extref", "extref.scd", xml, &model, &issues);

  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(model.ieds_size(), 1);
  ASSERT_EQ(model.ieds(0).ext_refs_size(), 1);
  const auto& extRef = model.ieds(0).ext_refs(0);
  EXPECT_EQ(extRef.access_point(), "AP1");
  EXPECT_EQ(extRef.owner_node_ref(), "SubscriberLD0/PTRC1");
  EXPECT_EQ(extRef.int_addr(), "trip-in");
  EXPECT_EQ(extRef.ied_name(), "Publisher");
  EXPECT_EQ(extRef.ld_inst(), "PROT");
  EXPECT_EQ(extRef.prefix(), "A");
  EXPECT_EQ(extRef.ln_class(), "PTRC");
  EXPECT_EQ(extRef.ln_inst(), "2");
  EXPECT_EQ(extRef.do_name(), "Tr");
  EXPECT_EQ(extRef.da_name(), "general");
  EXPECT_EQ(extRef.fc(), IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST);
  EXPECT_EQ(extRef.service_type(), "GOOSE");
  EXPECT_EQ(extRef.src_ld_inst(), "LD0");
  EXPECT_EQ(extRef.src_ln_class(), "LLN0");
  EXPECT_EQ(extRef.src_cb_name(), "gcbTrip");
  EXPECT_EQ(extRef.source_data_ref(),
            "PublisherPROT/APTRC2.Tr.general");
  EXPECT_EQ(IEC61850::SclParser::BuildSummary(model).external_reference_count(),
            1u);
}

// 验证：同一IED的两个Server AP即使使用相同标准对象引用，规范化对象仍保留各自AP归属。
TEST(IEC61850SclParserTest, PreservesAccessPointOwnershipForDuplicateReferences) {
  const std::string xml = R"xml(
<SCL xmlns="http://www.iec.ch/61850/2003/SCL">
  <IED name="IED1">
    <AccessPoint name="AP1"><Server><LDevice inst="LD0">
      <LN0 lnClass="LLN0" inst="" lnType="ln0">
        <DataSet name="events"><FCDA ldInst="LD0" lnClass="PTRC" lnInst="1" doName="Tr" daName="general" fc="ST"/></DataSet>
        <ReportControl name="brcb" datSet="events" buffered="true" confRev="1"/>
      </LN0>
      <LN lnClass="PTRC" inst="1" lnType="ptrc"/>
    </LDevice></Server></AccessPoint>
    <AccessPoint name="AP2"><Server><LDevice inst="LD0">
      <LN0 lnClass="LLN0" inst="" lnType="ln0">
        <DataSet name="events"><FCDA ldInst="LD0" lnClass="PTRC" lnInst="1" doName="Tr" daName="general" fc="ST"/></DataSet>
        <ReportControl name="brcb" datSet="events" buffered="true" confRev="2"/>
      </LN0>
      <LN lnClass="PTRC" inst="1" lnType="ptrc"/>
    </LDevice></Server></AccessPoint>
  </IED>
  <DataTypeTemplates>
    <LNodeType id="ln0" lnClass="LLN0"/>
    <LNodeType id="ptrc" lnClass="PTRC"><DO name="Tr" type="act"/></LNodeType>
    <DOType id="act" cdc="ACT"><DA name="general" bType="BOOLEAN" fc="ST"/></DOType>
  </DataTypeTemplates>
</SCL>)xml";
  IEC61850::SclParser parser;
  IEC61850Proto::NormalizedSclModel model;
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = parser.Parse("multi-ap", "multi-ap.scd", xml, &model,
                                   &issues);

  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(model.ieds_size(), 1);
  const auto& ied = model.ieds(0);
  ASSERT_EQ(ied.logical_nodes_size(), 4);
  EXPECT_EQ(ied.logical_nodes(0).access_point(), "AP1");
  EXPECT_EQ(ied.logical_nodes(2).access_point(), "AP2");
  ASSERT_EQ(ied.data_attributes_size(), 2);
  EXPECT_EQ(ied.data_attributes(0).data_ref(),
            ied.data_attributes(1).data_ref());
  EXPECT_EQ(ied.data_attributes(0).access_point(), "AP1");
  EXPECT_EQ(ied.data_attributes(1).access_point(), "AP2");
  ASSERT_EQ(ied.data_sets_size(), 2);
  EXPECT_EQ(ied.data_sets(0).data_set_ref(), ied.data_sets(1).data_set_ref());
  EXPECT_EQ(ied.data_sets(0).access_point(), "AP1");
  EXPECT_EQ(ied.data_sets(1).access_point(), "AP2");
  ASSERT_EQ(ied.report_controls_size(), 2);
  EXPECT_EQ(ied.report_controls(0).rcb_ref(),
            ied.report_controls(1).rcb_ref());
  EXPECT_EQ(ied.report_controls(0).config_revision(), 1u);
  EXPECT_EQ(ied.report_controls(1).config_revision(), 2u);
  EXPECT_EQ(ied.report_controls(0).access_point(), "AP1");
  EXPECT_EQ(ied.report_controls(1).access_point(), "AP2");
}

// 验证：一个AP中的同名DataSet不能满足另一个AP内控制块的数据集引用。
TEST(IEC61850SclParserTest, RejectsDataSetReferenceAcrossAccessPoints) {
  const std::string xml = R"xml(
<SCL xmlns="http://www.iec.ch/61850/2003/SCL">
  <IED name="IED1">
    <AccessPoint name="AP1"><Server><LDevice inst="LD0"><LN0 lnClass="LLN0" inst="" lnType="ln0">
      <DataSet name="events"/>
    </LN0></LDevice></Server></AccessPoint>
    <AccessPoint name="AP2"><Server><LDevice inst="LD0"><LN0 lnClass="LLN0" inst="" lnType="ln0">
      <ReportControl name="brcb" datSet="events" buffered="true"/>
    </LN0></LDevice></Server></AccessPoint>
  </IED>
  <DataTypeTemplates><LNodeType id="ln0" lnClass="LLN0"/></DataTypeTemplates>
</SCL>)xml";
  IEC61850::SclParser parser;
  IEC61850Proto::NormalizedSclModel model;
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = parser.Parse("cross-ap", "cross-ap.scd", xml, &model,
                                   &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "SCL_DANGLING_DATASET_REFERENCE");
  EXPECT_NE(issues.front().path().find("AP2"), std::string::npos);
}

// 验证：另一个AP中的同名DataSet不能满足GSEControl的数据集引用。
TEST(IEC61850SclParserTest, RejectsGseDataSetReferenceAcrossAccessPoints) {
  const std::string xml = R"xml(
<SCL xmlns="http://www.iec.ch/61850/2003/SCL">
  <IED name="IED1">
    <AccessPoint name="AP1"><Server><LDevice inst="LD0"><LN0 lnClass="LLN0" inst="" lnType="ln0">
      <DataSet name="events"/>
    </LN0></LDevice></Server></AccessPoint>
    <AccessPoint name="AP2"><Server><LDevice inst="LD0"><LN0 lnClass="LLN0" inst="" lnType="ln0">
      <GSEControl name="gcb" datSet="events"/>
    </LN0></LDevice></Server></AccessPoint>
  </IED>
  <DataTypeTemplates><LNodeType id="ln0" lnClass="LLN0"/></DataTypeTemplates>
</SCL>)xml";
  IEC61850::SclParser parser;
  IEC61850Proto::NormalizedSclModel model;
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = parser.Parse("cross-ap-gse", "cross-ap-gse.scd", xml,
                                   &model, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "SCL_DANGLING_DATASET_REFERENCE");
  EXPECT_NE(issues.front().path().find("GSEControl"), std::string::npos);
  EXPECT_NE(issues.front().path().find("AP2"), std::string::npos);
}

// 验证：另一个AP中的同名DataSet不能满足SampledValueControl的数据集引用。
TEST(IEC61850SclParserTest, RejectsSvDataSetReferenceAcrossAccessPoints) {
  const std::string xml = R"xml(
<SCL xmlns="http://www.iec.ch/61850/2003/SCL">
  <IED name="IED1">
    <AccessPoint name="AP1"><Server><LDevice inst="LD0"><LN0 lnClass="LLN0" inst="" lnType="ln0">
      <DataSet name="measurements"/>
    </LN0></LDevice></Server></AccessPoint>
    <AccessPoint name="AP2"><Server><LDevice inst="LD0"><LN0 lnClass="LLN0" inst="" lnType="ln0">
      <SampledValueControl name="smv" datSet="measurements"/>
    </LN0></LDevice></Server></AccessPoint>
  </IED>
  <DataTypeTemplates><LNodeType id="ln0" lnClass="LLN0"/></DataTypeTemplates>
</SCL>)xml";
  IEC61850::SclParser parser;
  IEC61850Proto::NormalizedSclModel model;
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = parser.Parse("cross-ap-sv", "cross-ap-sv.scd", xml,
                                   &model, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "SCL_DANGLING_DATASET_REFERENCE");
  EXPECT_NE(issues.front().path().find("SampledValueControl"),
            std::string::npos);
  EXPECT_NE(issues.front().path().find("AP2"), std::string::npos);
}

// 验证：同一IED内重复AccessPoint名称会被拒绝，避免Server模型归属产生歧义。
TEST(IEC61850SclParserTest, RejectsDuplicateAccessPointNames) {
  const std::string xml = R"xml(
<SCL xmlns="http://www.iec.ch/61850/2003/SCL">
  <IED name="IED1">
    <AccessPoint name="AP1"><Server/></AccessPoint>
    <AccessPoint name="AP1"><Server/></AccessPoint>
  </IED>
  <DataTypeTemplates/>
</SCL>)xml";
  IEC61850::SclParser parser;
  IEC61850Proto::NormalizedSclModel model;
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = parser.Parse("duplicate-ap", "duplicate-ap.scd", xml,
                                   &model, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "SCL_DUPLICATE_ACCESS_POINT");
}

// 验证：AccessPoint缺少名称时拒绝导入，避免与旧模型空归属哨兵混淆。
TEST(IEC61850SclParserTest, RejectsEmptyAccessPointName) {
  const std::string xml = R"xml(
<SCL xmlns="http://www.iec.ch/61850/2003/SCL">
  <IED name="IED1"><AccessPoint><Server/></AccessPoint></IED>
  <DataTypeTemplates/>
</SCL>)xml";
  IEC61850::SclParser parser;
  IEC61850Proto::NormalizedSclModel model;
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = parser.Parse("empty-ap", "empty-ap.scd", xml, &model,
                                   &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "SCL_ACCESS_POINT_NAME_EMPTY");
}

// 验证：展开数据属性数量恰好等于配置上限时允许导入。
TEST(IEC61850SclParserTest, AllowsExpandedAttributeCountEqualToLimit) {
  const std::string xml = R"xml(
<SCL xmlns="http://www.iec.ch/61850/2003/SCL">
  <IED name="IED1"><AccessPoint name="AP1"><Server><LDevice inst="LD0">
    <LN lnClass="MMXU" inst="1" lnType="ln1"/>
  </LDevice></Server></AccessPoint></IED>
  <DataTypeTemplates>
    <LNodeType id="ln1" lnClass="MMXU"><DO name="A" type="mv"/></LNodeType>
    <DOType id="mv" cdc="MV"><DA name="mag" bType="FLOAT32" fc="MX"/></DOType>
  </DataTypeTemplates>
</SCL>)xml";
  IEC61850::SclParser::Limits limits;
  limits.maxExpandedDataAttributes = 1;
  IEC61850::SclParser parser(limits);
  IEC61850Proto::NormalizedSclModel model;
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = parser.Parse("limit", "limit.scd", xml, &model, &issues);

  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(model.ieds_size(), 1);
  EXPECT_EQ(model.ieds(0).data_attributes_size(), 1);
}

// 验证：展开数据属性数量严格超过配置上限时拒绝导入。
TEST(IEC61850SclParserTest, RejectsExpandedAttributeCountOverLimit) {
  const std::string xml = R"xml(
<SCL xmlns="http://www.iec.ch/61850/2003/SCL">
  <IED name="IED1"><AccessPoint name="AP1"><Server><LDevice inst="LD0">
    <LN lnClass="MMXU" inst="1" lnType="ln1"/>
  </LDevice></Server></AccessPoint></IED>
  <DataTypeTemplates>
    <LNodeType id="ln1" lnClass="MMXU">
      <DO name="A" type="mv"/><DO name="B" type="mv"/>
    </LNodeType>
    <DOType id="mv" cdc="MV"><DA name="mag" bType="FLOAT32" fc="MX"/></DOType>
  </DataTypeTemplates>
</SCL>)xml";
  IEC61850::SclParser::Limits limits;
  limits.maxExpandedDataAttributes = 1;
  IEC61850::SclParser parser(limits);
  IEC61850Proto::NormalizedSclModel model;
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = parser.Parse("over-limit", "over-limit.scd", xml,
                                   &model, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED);
}

// 验证：ReportControl引用不存在的DataSet时返回可定位的校验错误。
TEST(IEC61850SclParserTest, RejectsDanglingReportDataSetReference) {
  const std::string xml = R"xml(
<SCL xmlns="http://www.iec.ch/61850/2003/SCL">
  <IED name="IED1">
    <AccessPoint name="AP1"><Server><LDevice inst="LD0">
      <LN0 lnClass="LLN0" inst="" lnType="ln0">
        <ReportControl name="r1" datSet="missing" buffered="false" confRev="1"/>
      </LN0>
    </LDevice></Server></AccessPoint>
  </IED>
  <DataTypeTemplates><LNodeType id="ln0" lnClass="LLN0"/></DataTypeTemplates>
</SCL>)xml";
  IEC61850::SclParser parser;
  IEC61850Proto::NormalizedSclModel model;
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = parser.Parse("broken", "broken.cid", xml, &model, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "SCL_DANGLING_DATASET_REFERENCE");
  EXPECT_NE(issues.front().path().find("ReportControl"), std::string::npos);
}

// 验证：重复IED名称会被拒绝，避免模型引用产生歧义。
TEST(IEC61850SclParserTest, RejectsDuplicateIedNames) {
  const std::string xml = R"xml(
<SCL xmlns="http://www.iec.ch/61850/2003/SCL">
  <IED name="IED1"/><IED name="IED1"/>
  <DataTypeTemplates/>
</SCL>)xml";
  IEC61850::SclParser parser;
  IEC61850Proto::NormalizedSclModel model;
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = parser.Parse("duplicate", "duplicate.icd", xml, &model, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "SCL_DUPLICATE_IED");
  EXPECT_EQ(model.document_kind(), IEC61850Proto::SCL_DOCUMENT_KIND_ICD);
}

// 验证：DAType直接或间接循环引用会返回可定位错误，不会无限递归或耗尽调用栈。
TEST(IEC61850SclParserTest, RejectsRecursiveDataAttributeTypes) {
  const std::string xml = R"xml(
  <SCL xmlns="http://www.iec.ch/61850/2003/SCL">
    <IED name="IED1"><AccessPoint name="AP1"><Server><LDevice inst="LD0">
      <LN lnClass="MMXU" inst="1" lnType="ln1"/>
    </LDevice></Server></AccessPoint></IED>
    <DataTypeTemplates>
      <LNodeType id="ln1" lnClass="MMXU"><DO name="TotW" type="do1"/></LNodeType>
      <DOType id="do1" cdc="MV"><DA name="mag" bType="Struct" type="cycleA" fc="MX"/></DOType>
      <DAType id="cycleA"><BDA name="next" bType="Struct" type="cycleB"/></DAType>
      <DAType id="cycleB"><BDA name="next" bType="Struct" type="cycleA"/></DAType>
    </DataTypeTemplates>
  </SCL>)xml";
  IEC61850::SclParser parser;
  IEC61850Proto::NormalizedSclModel model;
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = parser.Parse("recursive", "recursive.scd", xml,
                                   &model, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "SCL_RECURSIVE_DA_TYPE");
  EXPECT_NE(issues.front().path().find("DAType"), std::string::npos);
}

// 验证：空模型名、空来源或空内容在进入XML解析前即被拒绝。
TEST(IEC61850SclParserTest, RejectsMissingRequiredInput) {
  IEC61850::SclParser parser;
  IEC61850Proto::NormalizedSclModel model;
  std::vector<IEC61850Proto::ValidationIssue> issues;

  EXPECT_EQ(parser.Parse("", "station.scd", kCompleteScd, &model, &issues).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(parser.Parse("station", "", kCompleteScd, &model, &issues).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(parser.Parse("station", "station.scd", "", &model, &issues).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

}  // namespace
