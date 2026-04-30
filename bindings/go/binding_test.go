package tree_sitter_puppet_test

import (
	"testing"

	tree_sitter_puppet "github.com/smoeding/tree-sitter-puppet/bindings/go"
	tree_sitter "github.com/tree-sitter/go-tree-sitter"
)

func TestCanLoadGrammar(t *testing.T) {
	language := tree_sitter.NewLanguage(tree_sitter_puppet.Language())
	if language == nil {
		t.Errorf("Error loading Puppet grammar")
	}
}
