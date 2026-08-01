import re

filename = '.github/workflows/build_all.yml'
with open(filename, 'r') as f:
    content = f.read()

# Update actions/checkout to v4
content = re.sub(r'uses: actions/checkout@[^\s]+', 'uses: actions/checkout@v4', content)
# Update actions/upload-artifact to v4
content = re.sub(r'uses: actions/upload-artifact@[^\s]+', 'uses: actions/upload-artifact@v4', content)

with open(filename, 'w') as f:
    f.write(content)
