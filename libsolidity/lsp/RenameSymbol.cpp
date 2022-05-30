/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0
#include <libsolidity/lsp/RenameSymbol.h>
#include <libsolidity/lsp/Transport.h> // for RequestError
#include <libsolidity/lsp/Utils.h>
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTUtils.h>
#include <libsolidity/ast/ASTVisitor.h>

#include <fmt/format.h>

#include <memory>
#include <string>
#include <vector>

using namespace solidity::frontend;
using namespace solidity::langutil;
using namespace solidity::lsp;
using namespace std;

void RenameSymbol::operator()(MessageID _id, Json::Value const& _args)
{
	auto const&& [sourceUnitName, lineColumn] = extractSourceUnitNameAndLineColumn(_args);
	string const newName = _args["newName"].asString();
	string const uri = _args["textDocument"]["uri"].asString();

	ASTNode const* sourceNode = m_server.astNodeAtSourceLocation(sourceUnitName, lineColumn);

	m_symbolName = {};
	m_node = nullptr;
	m_sourceUnits = { &m_server.compilerStack().ast(sourceUnitName) };
	m_locations.clear();

	// Identify symbol name and node
	// TODO handling of UsingForDirective
	if (auto const* declaration = dynamic_cast<Declaration const*>(sourceNode))
	{
		optional<int> sourcePos = charStreamProvider()
			.charStream(sourceUnitName)
			.translateLineColumnToPosition(lineColumn);
		solAssert(sourcePos.has_value(), "Expected source pos");

		if (auto const* importDirective = dynamic_cast<ImportDirective const*>(declaration))
		{
			for (ImportDirective::SymbolAlias const& symbolAlias: importDirective->symbolAliases())
				if (symbolAlias.location.containsOffset(*sourcePos))
				{
					solAssert(symbolAlias.alias);
					m_symbolName = *symbolAlias.alias;
					m_node = importDirective;
					break;
				}
		}
		else if (declaration->nameLocation().containsOffset(*sourcePos)
		{
			m_symbolName = declaration->name();
			m_node = declaration;
		}
	}
	else if (auto const* identifier = dynamic_cast<Identifier const*>(sourceNode))
	{
		if (auto const* declReference = dynamic_cast<Declaration const*>(identifier->annotation().referencedDeclaration))
		{
			m_symbolName = identifier->name();
			m_node = declReference;
		}
	}
	else if (auto const* identifierPath = dynamic_cast<IdentifierPath const*>(sourceNode))
		if (auto const* declReference = dynamic_cast<Declaration const*>(identifierPath->annotation().referencedDeclaration))
		{
			m_node = declReference;
			m_symbolName = to_string(fmt::join(identifierPath->path(), "."));
		}

	// Find all source units using this symbol

	for (auto const& [name, content]: fileRepository().sourceUnits())
	{
		auto const& sourceUnit = m_server.compilerStack().ast(name);
			for (auto const* referencedSourceUnit: sourceUnit.referencedSourceUnits(true, util::convertContainer<set<SourceUnit const*>>(m_sourceUnits)))
				// TODO check for nullptr
				if (*referencedSourceUnit->location().sourceName == sourceUnitName)
				{
					m_sourceUnits.emplace_back(&sourceUnit);
					break;
				}
	}

	Visitor visitor(*this);

	for (auto const* sourceUnit: m_sourceUnits)
		sourceUnit->accept(visitor);

	// Apply changes in reverse order (will iterate in reverse)
	sort(m_locations.begin(), m_locations.end());

	Json::Value reply = Json::objectValue;
	reply["changes"] = Json::objectValue;

	Json::Value edits = Json::arrayValue;

	for (auto i = m_locations.rbegin(); i != m_locations.rend(); i++)
	{
		solAssert(i->isValid());

		// Replace in our file repository
		string const uri = fileRepository().sourceUnitNameToUri(*i->sourceName);
		string buffer = fileRepository().sourceUnits().at(*i->sourceName);
		buffer.replace((size_t)i->start, (size_t)(i->end - i->start), newName);
		fileRepository().setSourceByUri(uri, std::move(buffer));

		Json::Value edit = Json::objectValue;
		edit["range"] = toRange(*i);
		edit["newText"] = newName;

		// Record changes for the client
		edits.append(edit);
		if (i + 1 == m_locations.rend() || (i + 1)->sourceName != i->sourceName)
		{
			reply["changes"][uri] = edits;

			edits = Json::arrayValue;
		}
	}

	client().reply(_id, reply);
}

void RenameSymbol::Visitor::endVisit(frontend::ImportDirective const& _node)
{
	// If an import directive is to be renamed, it can only be because it
	// defines the symbol that is being renamed.
	if (&_node != m_outer.m_node)
		return;

	size_t const sizeBefore = m_outer.m_locations.size();

	for (frontend::ImportDirective::SymbolAlias const& symbolAlias: _node.symbolAliases())
		if (symbolAlias.alias != nullptr && *symbolAlias.alias == m_outer.m_symbolName)
			m_outer.m_locations.emplace_back(symbolAlias.location);

	solAssert(sizeBefore < m_outer.m_locations.size(), "Found no source location in ImportDirective?!");
}

void RenameSymbol::Visitor::endVisit(frontend::IdentifierPath const& _node)
{
	if (_node.
}
