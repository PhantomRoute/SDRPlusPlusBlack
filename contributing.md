# AI Use

Parts of this fork are written with AI assistance, so it would be odd to forbid
it here. Contributions written with an AI are fine, on one condition: you
understand what the code does and you have tested it on real hardware. Review it
the way you would review your own work, because once you submit it, that is what
it is.

**The projects upstream of this one take the opposite view.**
[SDR++](https://github.com/AlexandreRouma/SDRPlusPlus) forbids AI-generated
code, pull requests and issues outright, and says so in its own contributing
guide. Do not carry code from this fork into an upstream pull request without
rewriting it yourself and being straight with them about where it came from.
That policy is theirs to set and it deserves respect.

# Pull Requests

This is a personal fork, so it moves at the pace of one person. Open an issue
first for anything substantial - it saves you writing a patch against something
I was about to change. Small, focused fixes are welcome as pull requests
directly.

## Band Frequency Allocation 

Please follow this guide to properly format the JSON files for custom radio band allocation identifiers.

```json
{
    "name": "Short name (has to fit in the menu)",
    "country_name": "Name of country or area, if applicable (Use '--' otherwise)",
    "country_code": "Two letter country code, if applicable (Use '--' otherwise)",
    "author_name": "Name of the original/main creator of the JSON file",
    "author_url": "URL the author wishes to be associated with the file (personal website, GitHub, Twitter, etc)",
    "bands": [ 
        // Bands in this array must be sorted by their starting frequency
        {
            "name": "Name of the band",
            "type": "Type name ('amateur', 'broadcast', 'marine', 'military', or any type declared in config.json)",
            "start": 148500, //In Hz, must be an integer
            "end": 283500 //In Hz, must be an integer
        },
        {
            "name": "Name of the band",
            "type": "Type name ('amateur', 'broadcast', 'marine', 'military', or any type declared in config.json)",
            "start": 526500, //In Hz, must be an integer
            "end": 1606500 //In Hz, must be an integer
        }    
    ]
}
```

## Color Maps

Please follow this guide to properly format the JSON files for custom color maps.

```json
{
    "name": "Short name (has to fit in the menu)",
    "author": "Name of the original/main creator of the color map",
    "map": [
        // These are the color codes, in hexadecimal (#RRGGBB) format, for the custom color scales for the waterfall. They must be entered as strings, not integers, with the hashtag/pound-symbol proceeding the 6 digit number. 
        "#000020",
        "#000030",
        "#000050",
        "#000091",
        "#1E90FF",
        "#FFFFFF",
        "#FFFF00",
        "#FE6D16",
        "#FE6D16",
        "#FF0000",
        "#FF0000",
        "#C60000",
        "#9F0000",
        "#750000",
        "#4A0000"
    ]
}
```

# JSON Formatting

The ability to add new radio band allocation identifiers and color maps relies on JSON files. Proper formatting of these JSON files is important for reference and readability. The following guides will show you how to properly format the JSON files for their respective uses.

**IMPORTANT: JSON File cannot contain comments, there are only in this example for clarity**
