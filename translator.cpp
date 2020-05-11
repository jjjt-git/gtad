/******************************************************************************
 * Copyright (C) 2016 Kitsune Ral <kitsune-ral@users.sf.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "translator.h"

#include "analyzer.h"
#include "printer.h"

#include "yaml.h"

#include <QtCore/QDir>

#include <regex>

using namespace std;

void addTypeAttributes(TypeUsage& typeUsage, const YamlMap& attributesMap)
{
    for (const auto& attr: attributesMap)
    {
        auto attrName = attr.first.as<string>();
        if (attrName == "type")
            continue;
        switch (attr.second.Type())
        {
            case YAML::NodeType::Null:
                typeUsage.attributes.emplace(move(attrName), string{});
                break;
            case YAML::NodeType::Scalar:
                typeUsage.attributes.emplace(move(attrName),
                                             attr.second.as<string>());
                break;
            case YAML::NodeType::Sequence:
                if (const auto& seq = attr.second.asSequence())
                    typeUsage.lists.emplace(move(attrName), seq.asStrings());
                break;
            default:
                throw YamlException(attr.second, "Malformed attribute");
        }
    }
}

TypeUsage parseTargetType(const YamlNode& yamlTypeNode)
{
    using YAML::NodeType;
    if (yamlTypeNode.Type() == NodeType::Null)
        return {};
    if (yamlTypeNode.Type() == NodeType::Scalar)
        return TypeUsage(yamlTypeNode.as<string>());

    const auto yamlTypeMap = yamlTypeNode.asMap();
    TypeUsage typeUsage { yamlTypeMap["type"].as<string>("") };
    addTypeAttributes(typeUsage, yamlTypeMap);
    return typeUsage;
}

TypeUsage parseTargetType(const YamlNode& yamlTypeNode,
                          const YamlMap& commonAttributesYaml)
{
    auto tu = parseTargetType(yamlTypeNode);
    addTypeAttributes(tu, commonAttributesYaml);
    return tu;
}

template <typename FnT>
auto parseEntries(const YamlSequence& entriesYaml, const FnT& inserter,
                  const YamlMap& commonAttributesYaml = {})
    -> enable_if_t<is_void_v<decltype(
        inserter(string(), YamlNode(), YamlMap()))>>
{
    for (const YamlMap typesBlockYaml: entriesYaml)
    {
        switch (typesBlockYaml.size())
        {
            case 0:
                throw YamlException(typesBlockYaml, "Empty type entry");
            case 1:
            {
                const auto& typeYaml = typesBlockYaml.front();
                inserter(typeYaml.first.as<string>(),
                         typeYaml.second, commonAttributesYaml);
                break;
            }
            case 2:
                if (typesBlockYaml["+on"] && typesBlockYaml["+set"])
                {
                    parseEntries(typesBlockYaml.get("+on").asSequence(),
                                 inserter, typesBlockYaml.get("+set").asMap());
                    break;
                }
                // FALLTHROUGH
            default:
                throw YamlException(typesBlockYaml,
                        "Too many entries in the map, check indentation");
        }
    }
}

pair_vector_t<TypeUsage> parseTypeEntry(const YamlNode& targetTypeYaml,
                                        const YamlMap& commonAttributesYaml = {})
{
    switch (targetTypeYaml.Type())
    {
        case YAML::NodeType::Scalar: // Use a type with no regard to format
        case YAML::NodeType::Map: // Same, with attributes for the target type
        {
            return { { {},
                       parseTargetType(targetTypeYaml, commonAttributesYaml) } };
        }
        case YAML::NodeType::Sequence: // A list of formats for the type
        {
            pair_vector_t<TypeUsage> targetTypes;
            parseEntries(targetTypeYaml.asSequence(),
                [&targetTypes](string formatName,
                    const YamlNode& typeYaml, const YamlMap& commonAttrsYaml)
                {
                    if (formatName.empty())
                        formatName = "/"; // Empty format means all formats
                    else if (formatName.size() > 1 &&
                             formatName.front() == '/' &&
                             formatName.back() == '/')
                    {
                        formatName.pop_back();
                    }
                    targetTypes.emplace_back(move(formatName),
                        parseTargetType(typeYaml, commonAttrsYaml));
                }, commonAttributesYaml);
            return targetTypes;
        }
        default:
            throw YamlException(targetTypeYaml, "Malformed type entry");
    }
}

pair_vector_t<string> loadStringMap(const YamlMap& yaml)
{
    pair_vector_t<string> stringMap;
    for (const auto& subst: yaml)
    {
        auto pattern = subst.first.as<string>();
        if (Q_UNLIKELY(pattern.empty()))
            clog << subst.first.location()
                 << ": warning: empty pattern in substitutions, skipping"
                 << endl;
        else if (Q_UNLIKELY(pattern.size() > 1 &&
                            pattern.front() != '/' && pattern.back() == '/'))
            clog << subst.first.location()
                 << ": warning: invalid regular expression, skipping" << endl
                 << "(use a regex with \\/ to match strings beginning with /)";
        else
        {
            if (pattern.front() == '/' && pattern.back() == '/')
                pattern.pop_back();
            stringMap.emplace_back(pattern, subst.second.as<string>());
        }
    }
    return stringMap;
}

Translator::Translator(const QString& configFilePath, QString outputDirPath)
    : _outputDirPath(outputDirPath.endsWith('/') ?
                     move(outputDirPath) : outputDirPath + '/')
{
    auto cfp = configFilePath.toStdString();
    cout << "Using config file at " << cfp << endl;
    const auto configY = YamlMap::loadFromFile(cfp);

    const auto& analyzerYaml = configY["analyzer"].asMap();
    _substitutions = loadStringMap(analyzerYaml["subst"].asMap());
    _identifiers = loadStringMap(analyzerYaml["identifiers"].asMap());

    parseEntries(analyzerYaml["types"].asSequence(),
        [this](const string& name, const YamlNode& typeYaml,
               const YamlMap& commonAttrsYaml)
        {
            _typesMap.emplace_back(name,
                parseTypeEntry(typeYaml, commonAttrsYaml));
        });

    Printer::context_type env;
    using namespace kainjow::mustache;
    const auto& mustacheYaml = configY["mustache"].asMap();
    const auto& envYaml = mustacheYaml["constants"].asMap();
    for (const auto& p: envYaml)
    {
        const auto pName = p.first.as<string>();
        if (p.second.IsScalar())
        {
            env.set(pName, p.second.as<string>());
            continue;
        }
        const auto pDefinition = p.second.asMap().front();
        const auto pType = pDefinition.first.as<string>();
        const YamlNode defaultVal = pDefinition.second;
        if (pType == "set")
            env.set(pName, { data::type::list });
        else if (pType == "bool")
            env.set(pName, { defaultVal.as<bool>() });
        else
            env.set(pName, { defaultVal.as<string>() });
    }
    const auto& partialsYaml = mustacheYaml["partials"].asMap();
    for (const auto& p: partialsYaml)
    {
        env.set(p.first.as<string>(),
                partial { [s = p.second.as<string>()] { return s; } });

    }

    vector<string> outputFiles;
    const auto& templatesYaml = mustacheYaml["templates"].asSequence();
    for (const auto& f: templatesYaml)
        outputFiles.emplace_back(f.as<string>());

    QString configDir = QFileInfo(configFilePath).dir().path();
    if (!configDir.isEmpty())
        configDir += '/';

    _printer = std::make_unique<Printer>(
        move(env), outputFiles, configDir.toStdString(),
        _outputDirPath.toStdString(),
        mustacheYaml["outFilesList"].as<string>(""));
}

Translator::~Translator() = default;

TypeUsage Translator::mapType(const string& swaggerType,
                              const string& swaggerFormat,
                              const string& baseName) const
{
    TypeUsage tu;
    for (const auto& swTypePair: _typesMap)
        if (swTypePair.first == swaggerType)
            for (const auto& swFormatPair: swTypePair.second)
            {
                const auto& swFormat = swFormatPair.first;
                if (swFormat == swaggerFormat ||
                    (!swFormat.empty() && swFormat.front() == '/' &&
                     regex_search(swaggerFormat,
                                  regex(++swFormat.begin(), swFormat.end()))))
                {
                    // FIXME (#22): a source of great inefficiency.
                    // TypeUsage should become a handle to an instance of
                    // a newly-made TypeDefinition type that would own all
                    // the stuff TypeUsage now has, except paramTypes
                    tu = swFormatPair.second;
                    goto conclusion;
                }
            }
conclusion:
    // Fallback chain: baseName, swaggerFormat, swaggerType
    tu.baseName = baseName.empty() ? swaggerFormat.empty() ?
                                     swaggerType : swaggerFormat
                                   : baseName;
    return tu;
}

string Translator::mapIdentifier(const string& baseName,
                                 const string& scope) const
{
    auto scopedName = scope;
    scopedName.append(1, '/').append(baseName);
    for (const auto& entry: _identifiers)
    {
        const auto& pattn = entry.first;
        if (!pattn.empty() && pattn.front() == '/')
            return regex_replace(scopedName,
                                 regex(++pattn.begin(), pattn.end()),
                                 entry.second);

        if (pattn == baseName || pattn == scopedName)
            return entry.second;
    }
    return baseName;
}

Model&& Translator::processFile(string filePath, string baseDirPath,
                                InOut inOut, bool skipTrivial) const
{
    auto&& m = Analyzer(move(filePath), move(baseDirPath), *this)
                .loadModel(_substitutions, inOut);
    if (!(m.empty() || (m.trivial() && skipTrivial))) {
        QDir d{_outputDirPath + m.fileDir.c_str()};
        if (!d.exists() && !d.mkpath("."))
            throw Exception{"Cannot create output directory"};
        m.dstFiles = _printer->print(m);
    }

    return move(m);
}

