---
title: Updates
nav_order: 10
---

# Project updates

Monthly notes on what changed and what's next. Newest first.

{% for post in site.posts %}
- **[{{ post.title }}]({{ post.url | relative_url }})** ({{ post.date | date: "%B %Y" }}): {{ post.excerpt | strip_html | truncatewords: 40 }}
{% endfor %}

<!-- A new update: docs/_posts/YYYY-MM-DD-title.md with front matter
     "title:" and "layout: default"; the first paragraph is the excerpt. -->
