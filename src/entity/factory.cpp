//
// Copyright Copyright 2009-2019, AMT – The Association For Manufacturing Technology (“AMT”)
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

#include "factory.hpp"

#include <dlib/logger.h>

using namespace std;

namespace mtconnect
{
  namespace entity
  {
    static dlib::logger g_logger("EntityFactory");

    void Factory::_dupFactory(FactoryPtr &factory, FactoryMap &factories)
    {
      auto old = factories.find(factory);
      if (old != factories.end())
      {
        factory = old->second;
      }
      else
      {
        auto ptr = make_shared<Factory>(*factory);
        ptr->_deepCopy(factories);
        factories.emplace(factory, ptr);
        factory = ptr;
      }
    }

    void Factory::_deepCopy(FactoryMap &factories)
    {
      for (auto &r : m_requirements)
      {
        auto factory = r.getFactory();
        if (factory)
        {
          _dupFactory(factory, factories);
          r.setFactory(factory);
        }
      }

      for (auto &f : m_regexFactory)
      {
        _dupFactory(f.second, factories);
      }

      for (auto &f : m_stringFactory)
      {
        _dupFactory(f.second, factories);
      }
    }

    FactoryPtr Factory::deepCopy() const
    {
      auto copy = make_shared<Factory>(*this);
      map<FactoryPtr, FactoryPtr> factories;
      copy->_deepCopy(factories);

      return copy;
    }

    void Factory::LogError(const std::string &what) { g_logger << dlib::LWARN << what; }

    void Factory::performConversions(Properties &properties, ErrorList &errors) const
    {
      for (const auto &r : m_requirements)
      {
        if (r.getType() != ENTITY && r.getType() != ENTITY_LIST)
        {
          const auto p = properties.find(r.getName());
          if (p != properties.end() && p->second.index() != r.getType())
          {
            try
            {
              Value &v = p->second;
              ConvertValueToType(v, r.getType());
            }
            catch (PropertyError &e)
            {
              g_logger << dlib::LWARN << "Error occurred converting " << r.getName() << ": "
                       << e.what();
              e.setProperty(r.getName());
              errors.emplace_back(e.dup());
              properties.erase(p);
            }
          }
        }
      }
    }

    bool Factory::isSufficient(Properties &properties, ErrorList &errors) const
    {
      std::set<std::string> keys;
      std::transform(properties.begin(), properties.end(), std::inserter(keys, keys.end()),
                     [](const auto &v) { return v.first; });
      bool success{true};
      for (const auto &r : m_requirements)
      {
        std::string key;
        if (m_isList && r.getType() == ENTITY)
          key = "LIST";
        else
          key = r.getName();
        const auto p = properties.find(key);
        if (p == properties.end())
        {
          if (r.isRequired())
          {
            errors.emplace_back(new PropertyError(
                "Property " + r.getName() + " is required and not provided", r.getName()));
            success = false;
          }
        }
        else
        {
          try
          {
            if (!r.isMetBy(p->second, m_isList))
            {
              success = false;
            }
          }
          catch (PropertyError &e)
          {
            LogError(e.what());
            if (r.isRequired())
            {
              success = false;
            }
            else
            {
              LogError("Not required, skipping " + r.getName());
              properties.erase(p->first);
            }
            e.setProperty(r.getName());
            errors.emplace_back(e.dup());
          }
          keys.erase(r.getName());
        }
      }

      // Check for additional properties
      if (!m_isList && !keys.empty())
      {
        std::stringstream os;
        os << "The following keys were present and not expected: ";
        for (auto &k : keys)
          os << k << ",";
        errors.emplace_back(new PropertyError(os.str()));
        success = false;
      }

      return success;
    }
  }  // namespace entity
}  // namespace mtconnect