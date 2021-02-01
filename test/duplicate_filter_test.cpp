//
// Copyright Copyright 2009-2021, AMT – The Association For Manufacturing Technology (“AMT”)
// All rights reserved.
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//    Unless required by applicable law or agreed to in writing, software
//    distributed under the License is distributed on an "AS IS" BASIS,
//    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//    See the License for the specific language governing permissions and
//    limitations under the License.
//

// Ensure that gtest is the first header otherwise Windows raises an error
#include <gtest/gtest.h>
// Keep this comment to keep gtest.h above. (clang-format off/on is not working here!)

#include "pipeline/pipeline.hpp"
#include "pipeline/shdr_token_mapper.hpp"
#include "pipeline/duplicate_filter.hpp"
#include "pipeline/delta_filter.hpp"
#include "pipeline/period_filter.hpp"
#include "observation/observation.hpp"
#include <chrono>

using namespace mtconnect;
using namespace mtconnect::pipeline;
using namespace mtconnect::observation;
using namespace std;
using namespace std::literals;
using namespace std::chrono_literals;

class MockPipelineContract : public PipelineContract
{
public:
  MockPipelineContract(std::map<string,unique_ptr<DataItem>> &items)
  : m_dataItems(items)
  {
  }
  DataItem *findDataItem(const std::string &device, const std::string &name) override
  {
    return m_dataItems[name].get();
  }
  void eachDataItem(EachDataItem fun) override {}
  void deliverObservation(observation::ObservationPtr obs) override {}
  void deliverAsset(AssetPtr )override {}
  void deliverAssetCommand(entity::EntityPtr ) override {}
  void deliverCommand(entity::EntityPtr )override {}
  void deliverConnectStatus(entity::EntityPtr )override {}
  
  std::map<string,unique_ptr<DataItem>> &m_dataItems;
};

class DuplicateFilterTest : public testing::Test
{
protected:
  void SetUp() override
  {
    m_context = make_shared<PipelineContext>();
    m_context->m_contract = make_unique<MockPipelineContract>(m_dataItems);
    m_mapper = make_shared<ShdrTokenMapper>(m_context);
    m_mapper->bind(make_shared<NullTransform>(TypeGuard<Observations>(RUN)));
  }


  void TearDown() override
  {
    m_dataItems.clear();
  }
  
  DataItem *makeDataItem(std::map<string,string> attributes)
  {
    auto di = make_unique<DataItem>(attributes);
    DataItem *r = di.get();
    m_dataItems.emplace(attributes["id"], move(di));
    
    return r;
  }
  
  
  const EntityPtr observe(TokenList tokens, Timestamp now = chrono::system_clock::now())
  {
    auto ts = make_shared<Timestamped>();
    ts->m_tokens = tokens;
    ts->m_timestamp = now;
    ts->setProperty("timestamp", ts->m_timestamp);

    return (*m_mapper)(ts);
  }
 
  shared_ptr<ShdrTokenMapper> m_mapper;
  std::map<string,unique_ptr<DataItem>> m_dataItems;
  shared_ptr<PipelineContext> m_context;
};

TEST_F(DuplicateFilterTest, test_simple_event)
{
  makeDataItem({{"id", "a"}, {"type", "EXECUTION"}, {"category", "EVENT"}});

  auto filter = make_shared<DuplicateFilter>(m_context);
  m_mapper->bind(filter);

  auto os1 = observe({"a", "READY"});
  auto list1 = os1->getValue<EntityList>();
  ASSERT_EQ(1, list1.size());
  
  auto os2 = observe({"a", "READY"});
  auto list2 = os2->getValue<EntityList>();
  ASSERT_EQ(0, list2.size());

  auto os3 = observe({"a", "ACTIVE"});
  auto list3 = os3->getValue<EntityList>();
  ASSERT_EQ(1, list3.size());
}

TEST_F(DuplicateFilterTest, test_simple_sample)
{
  makeDataItem({{"id", "a"}, {"type", "POSITION"}, {"category", "SAMPLE"},
    {"units", "MILLIMETER"}
  });

  auto filter = make_shared<DuplicateFilter>(m_context);
  m_mapper->bind(filter);

  auto os1 = observe({"a", "1.5"});
  auto list1 = os1->getValue<EntityList>();
  ASSERT_EQ(1, list1.size());
  
  auto os2 = observe({"a", "1.5"});
  auto list2 = os2->getValue<EntityList>();
  ASSERT_EQ(0, list2.size());

  auto os3 = observe({"a", "1.6"});
  auto list3 = os3->getValue<EntityList>();
  ASSERT_EQ(1, list3.size());
}

TEST_F(DuplicateFilterTest, test_minimum_delta)
{
  makeDataItem({{"id", "a"}, {"type", "POSITION"}, {"category", "SAMPLE"},
    {"units", "MILLIMETER"}
  });
  
  auto filter = make_shared<DuplicateFilter>(m_context);
  m_mapper->bind(filter);

  auto rate = make_shared<DeltaFilter>(m_context);
  rate->addMinimumDelta("a", 1.0);
  filter->bind(rate);

  {
    auto os = observe({"a", "1.5"});
    auto list = os->getValue<EntityList>();
    ASSERT_EQ(1, list.size());
  }
  {
    auto os = observe({"a", "1.6"});
    auto list = os->getValue<EntityList>();
    ASSERT_EQ(0, list.size());
  }
  {
    auto os = observe({"a", "1.8"});
    auto list = os->getValue<EntityList>();
    ASSERT_EQ(0, list.size());
  }
  {
    auto os = observe({"a", "2.8"});
    auto list = os->getValue<EntityList>();
    ASSERT_EQ(1, list.size());
  }
  {
    auto os = observe({"a", "2.0"});
    auto list = os->getValue<EntityList>();
    ASSERT_EQ(0, list.size());
  }
  {
    auto os = observe({"a", "1.7"});
    auto list = os->getValue<EntityList>();
    ASSERT_EQ(1, list.size());
  }
}

TEST_F(DuplicateFilterTest, test_period_filter)
{
  makeDataItem({{"id", "a"}, {"type", "POSITION"}, {"category", "SAMPLE"},
    {"units", "MILLIMETER"}
  });
  
  Timestamp now = chrono::system_clock::now();

  auto rate = make_shared<PeriodFilter>(m_context);
  rate->addMinimumDuration("a", 10.0s);
  m_mapper->bind(rate);
  
  {
    auto os = observe({"a", "1.5"}, now);
    auto list = os->getValue<EntityList>();
    ASSERT_EQ(1, list.size());
  }
  {
    auto os = observe({"a", "1.5"}, now + 2s);
    auto list = os->getValue<EntityList>();
    ASSERT_EQ(0, list.size());
  }
  {
    auto os = observe({"a", "1.5"}, now + 5s);
    auto list = os->getValue<EntityList>();
    ASSERT_EQ(0, list.size());
  }
  {
    auto os = observe({"a", "1.5"}, now + 11s);
    auto list = os->getValue<EntityList>();
    ASSERT_EQ(1, list.size());
  }

}
