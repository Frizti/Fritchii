"""Check the pt_BR catalog against the Qt messages extracted from this build."""

from collections import Counter
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET


def messages(path):
    by_key = {}
    for context in ET.parse(path).findall("context"):
        for message in context.findall("message"):
            key = (context.findtext("name"), message.findtext("source"),
                   message.findtext("comment"), message.get("numerus"))
            if key in by_key:
                raise ValueError(f"Duplicate message in {path}: {key}")
            by_key[key] = message
    return by_key


def placeholders(text):
    return Counter(re.findall(r"%(?:L?\d+|L?n|[A-Z_]{2,}%)", text))


def check(source_path, catalog_path):
    source = messages(source_path)
    catalog = messages(catalog_path)
    for key in source:
        if key not in catalog:
            raise ValueError(f"Missing Brazilian Portuguese translation: {key}")
    for key, message in catalog.items():
        context, original, _, plural = key
        translation = message.find("translation")
        if translation is None or translation.get("type"):
            raise ValueError(f"Unfinished translation: {context}: {original}")
        forms = [item.text for item in translation.findall("numerusform")] if plural else [translation.text]
        if len(forms) != (2 if plural else 1):
            raise ValueError(f"Unexpected number of plural forms: {context}: {original}")
        for form in forms:
            if not form or placeholders(original) != placeholders(form):
                raise ValueError(f"Empty translation or placeholder mismatch: {context}: {original}")
    print(f"{len(source)} extracted messages covered, {len(catalog)} translations checked.")


if __name__ == "__main__":
    check(Path(sys.argv[1]), Path(sys.argv[2]))
