#include "list.h" 
#include "../debug.h"

/* 우리 더블 링크드 리스트는 두 개의 헤더 요소를 가지고 있다: 
   첫 번째 요소 바로 앞에 있는 "head"와 마지막 요소 바로 뒤에 있는 "tail".
   리스트의 첫 번째 헤더의 `prev` 링크는 NULL이며, 마지막 헤더의 `next` 링크도 NULL이다.
   이 외의 다른 두 링크는 리스트 내부의 요소들을 통해 서로를 가리키고 있다.

   빈 리스트의 모양은 다음과 같다:

   +------+     +------+
   <---| head |<--->| tail |--->
   +------+     +------+

   요소가 두 개 있는 리스트의 모양은 다음과 같다:

   +------+     +-------+     +-------+     +------+
   <---| head |<--->|   1   |<--->|   2   |<--->| tail |<--->
   +------+     +-------+     +-------+     +------+

   이러한 대칭 구조는 리스트 처리를 할 때 많은 특수한 경우를 제거해준다. 
   예를 들어, list_remove() 함수를 보면, 두 개의 포인터 할당만으로 충분하며 조건문이 필요 없다.
   이는 헤더 요소가 없을 때보다 훨씬 간단하다.

   (헤더 요소들 중 하나의 포인터만 실제로 사용되기 때문에, 
   헤더 요소를 하나로 합쳐도 이런 간결성을 잃지 않는다. 
   그러나 두 개의 개별 요소를 사용함으로써 몇 가지 연산에서 검사 기능을 추가할 수 있어 유용하다.) */

static bool is_sorted (struct list_elem *a, struct list_elem *b,
		list_less_func *less, void *aux) UNUSED;

/* ELEM이 head인지 여부를 반환. */
static inline bool
is_head (struct list_elem *elem) {
	return elem != NULL && elem->prev == NULL && elem->next != NULL;
}

/* ELEM이 리스트의 내부 요소인지 여부를 반환. */
static inline bool
is_interior (struct list_elem *elem) {
	return elem != NULL && elem->prev != NULL && elem->next != NULL;
}

/* ELEM이 tail인지 여부를 반환. */
static inline bool
is_tail (struct list_elem *elem) {
	return elem != NULL && elem->prev != NULL && elem->next == NULL;
}

/* LIST를 빈 리스트로 초기화. */
void
list_init (struct list *list) {
	ASSERT (list != NULL);
	list->head.prev = NULL;
	list->head.next = &list->tail;
	list->tail.prev = &list->head;
	list->tail.next = NULL;
}

/* LIST의 첫 번째 요소를 반환. */
struct list_elem *
list_begin (struct list *list) {
	ASSERT (list != NULL);
	return list->head.next;
}

/* ELEM 다음에 위치한 요소를 반환. 
   ELEM이 리스트의 마지막 요소일 경우 리스트의 tail을 반환. 
   ELEM이 리스트의 tail일 경우 결과는 정의되지 않음. */
struct list_elem *
list_next (struct list_elem *elem) {
	ASSERT (is_head (elem) || is_interior (elem));
	return elem->next;
}

/* LIST의 tail을 반환.

   list_end()는 주로 리스트를 앞에서부터 뒤로 순회할 때 사용된다. */
struct list_elem *
list_end (struct list *list) {
	ASSERT (list != NULL);
	return &list->tail;
}

/* LIST의 끝에서부터 시작하여 역방향으로 순회할 수 있도록
   LIST의 역방향 시작을 반환. */
struct list_elem *
list_rbegin (struct list *list) {
	ASSERT (list != NULL);
	return list->tail.prev;
}

/* ELEM 이전에 위치한 요소를 반환. 
   ELEM이 리스트의 첫 번째 요소일 경우 리스트의 head를 반환. 
   ELEM이 리스트의 head일 경우 결과는 정의되지 않음. */
struct list_elem *
list_prev (struct list_elem *elem) {
	ASSERT (is_interior (elem) || is_tail (elem));
	return elem->prev;
}

/* LIST의 head를 반환.

   list_rend()는 리스트를 역방향으로 순회할 때 주로 사용된다. */
struct list_elem *
list_rend (struct list *list) {
	ASSERT (list != NULL);
	return &list->head;
}

/* LIST의 head를 반환.

   list_head()는 리스트를 순회할 때의 다른 스타일로 사용될 수 있다. */
struct list_elem *
list_head (struct list *list) {
	ASSERT (list != NULL);
	return &list->head;
}

/* LIST의 tail을 반환. */
struct list_elem *
list_tail (struct list *list) {
	ASSERT (list != NULL);
	return &list->tail;
}

/* BEFORE 바로 앞에 ELEM을 삽입. 
   BEFORE는 리스트의 내부 요소이거나 tail일 수 있으며, 후자의 경우 list_push_back()과 동일. */
void
list_insert (struct list_elem *before, struct list_elem *elem) {
	ASSERT (is_interior (before) || is_tail (before));
	ASSERT (elem != NULL);

	elem->prev = before->prev;
	elem->next = before;
	before->prev->next = elem;
	before->prev = elem;
}

/* FIRST부터 LAST까지의 요소들을 현재 리스트에서 제거하고,
   BEFORE 바로 앞에 삽입. BEFORE는 리스트의 내부 요소이거나 tail일 수 있다. */
void
list_splice (struct list_elem *before,
		struct list_elem *first, struct list_elem *last) {
	ASSERT (is_interior (before) || is_tail (before));
	if (first == last)
		return;
	last = list_prev (last);

	ASSERT (is_interior (first));
	ASSERT (is_interior (last));

	/* FIRST...LAST를 현재 리스트에서 깨끗하게 제거. */
	first->prev->next = last->next;
	last->next->prev = first->prev;

	/* FIRST...LAST를 새로운 리스트에 스플라이스. */
	first->prev = before->prev;
	last->next = before;
	before->prev->next = first;
	before->prev = last;
}

/* LIST의 맨 앞에 ELEM을 삽입하여 LIST의 맨 앞 요소가 되도록 함. */
void
list_push_front (struct list *list, struct list_elem *elem) {
	list_insert (list_begin (list), elem);
}

/* LIST의 맨 뒤에 ELEM을 삽입하여 LIST의 맨 끝 요소가 되도록 함. */
void
list_push_back (struct list *list, struct list_elem *elem) {
	list_insert (list_end (list), elem);
}


/* ELEM을 리스트에서 제거하고, 제거된 ELEM의 다음 요소를 반환. 
   ELEM이 리스트에 속하지 않는 경우에는 정의되지 않은 동작을 초래함.

   ELEM을 제거한 후에는 리스트의 요소로 간주하지 않는 것이 안전함. 
   특히, list_next()나 list_prev()를 제거 후 사용하면 정의되지 않은 동작을 초래함. 
   따라서 리스트에서 요소를 제거하는 반복문을 작성할 때 다음과 같이 작성하지 말아야 함:

 ** 이런 방식은 사용하지 마세요 **
 for (e = list_begin (&list); e != list_end (&list); e = list_next (e))
 {
   ...do something with e...
   list_remove (e);
 }

 다음은 올바른 방식으로 요소를 제거하며 반복하는 방법임:

 for (e = list_begin (&list); e != list_end (&list); e = list_remove (e))
 {
   ...do something with e...
 }

 만약 리스트 요소를 free()해야 하는 경우라면 더 신중해야 함. 
 다음의 대안 전략은 이러한 경우에도 잘 작동함:

 while (!list_empty (&list))
 {
   struct list_elem *e = list_pop_front (&list);
   ...do something with e...
 }
*/
struct list_elem *
list_remove (struct list_elem *elem) {
	ASSERT (is_interior (elem));
	elem->prev->next = elem->next;
	elem->next->prev = elem->prev;
	return elem->next;
}

/* LIST의 맨 앞 요소를 제거하고 반환.
   LIST가 빈 경우 제거 전에는 정의되지 않은 동작을 초래함. */
struct list_elem *
list_pop_front (struct list *list) {
	struct list_elem *front = list_front (list);
	list_remove (front);
	return front;
}

/* LIST의 맨 뒤 요소를 제거하고 반환.
   LIST가 빈 경우 제거 전에는 정의되지 않은 동작을 초래함. */
struct list_elem *
list_pop_back (struct list *list) {
	struct list_elem *back = list_back (list);
	list_remove (back);
	return back;
}

/* LIST의 맨 앞 요소를 반환.
   LIST가 비어 있는 경우 정의되지 않은 동작을 초래함. */
struct list_elem *
list_front (struct list *list) {
	ASSERT (!list_empty (list));
	return list->head.next;
}

/* LIST의 맨 뒤 요소를 반환.
   LIST가 비어 있는 경우 정의되지 않은 동작을 초래함. */
struct list_elem *
list_back (struct list *list) {
	ASSERT (!list_empty (list));
	return list->tail.prev;
}

/* LIST의 요소 개수를 반환.
   LIST의 요소 개수에 따라 O(n) 시간이 소요됨. */
size_t
list_size (struct list *list) {
	struct list_elem *e;
	size_t cnt = 0;

	for (e = list_begin (list); e != list_end (list); e = list_next (e))
		cnt++;
	return cnt;
}

/* LIST가 비어 있으면 true를, 그렇지 않으면 false를 반환. */
bool
list_empty (struct list *list) {
	return list_begin (list) == list_end (list);
}

/* A와 B가 가리키는 `struct list_elem *'를 서로 교환. */
static void
swap (struct list_elem **a, struct list_elem **b) {
	struct list_elem *t = *a;
	*a = *b;
	*b = t;
}

/* LIST의 순서를 반대로 뒤집음. */
void
list_reverse (struct list *list) {
	if (!list_empty (list)) {
		struct list_elem *e;

		for (e = list_begin (list); e != list_end (list); e = e->prev)
			swap (&e->prev, &e->next);
		swap (&list->head.next, &list->tail.prev);
		swap (&list->head.next->prev, &list->tail.prev->next);
	}
}

/* 리스트 요소 A에서 B (exclusive)까지가 
   주어진 보조 데이터 AUX를 기준으로 LESS에 따라 정렬된 경우 true를 반환. */
static bool
is_sorted (struct list_elem *a, struct list_elem *b,
		list_less_func *less, void *aux) {
	if (a != b)
		while ((a = list_next (a)) != b)
			if (less (a, list_prev (a), aux))
				return false;
	return true;
}

/* A에서 시작해 B를 넘지 않는 범위 내에서 LESS와 AUX를 기준으로 
   비내림차순의 리스트 요소로 구성된 구간을 찾음. 
   구간의 (exclusive) 끝을 반환.
   A에서 B까지 (exclusive)는 비어 있지 않은 범위를 형성해야 함. */
static struct list_elem *
find_end_of_run (struct list_elem *a, struct list_elem *b,
		list_less_func *less, void *aux) {
	ASSERT (a != NULL);
	ASSERT (b != NULL);
	ASSERT (less != NULL);
	ASSERT (a != b);

	do {
		a = list_next (a);
	} while (a != b && !less (a, list_prev (a), aux));
	return a;
}

/* A0부터 A1B0 (exclusive)까지와 A1B0부터 B1 (exclusive)까지를 병합하여 
   B1 (exclusive)에서 끝나는 결합된 범위를 형성. 
   두 입력 범위는 비어 있지 않으며 LESS와 AUX에 따라 비내림차순으로 정렬되어야 함. 
   출력 범위도 동일하게 정렬됨. */
static void
inplace_merge (struct list_elem *a0, struct list_elem *a1b0,
		struct list_elem *b1,
		list_less_func *less, void *aux) {
	ASSERT (a0 != NULL);
	ASSERT (a1b0 != NULL);
	ASSERT (b1 != NULL);
	ASSERT (less != NULL);
	ASSERT (is_sorted (a0, a1b0, less, aux));
	ASSERT (is_sorted (a1b0, b1, less, aux));

	while (a0 != a1b0 && a1b0 != b1)
		if (!less (a1b0, a0, aux))
			a0 = list_next (a0);
		else {
			a1b0 = list_next (a1b0);
			list_splice (a0, list_prev (a1b0), a1b0);
		}
}

/* LIST를 LESS와 AUX에 따라 정렬. 
   자연스럽고 반복적인 병합 정렬을 사용하여 O(n log n) 시간과 
   O(1) 공간 복잡도로 LIST의 요소들을 정렬함. */
void
list_sort (struct list *list, list_less_func *less, void *aux) {
	size_t output_run_cnt; /* 현재 패스에서 출력된 run의 수. */

	ASSERT (list != NULL);
	ASSERT (less != NULL);

	/* 리스트를 반복적으로 확인하여 비내림차순 요소의 
	   인접한 run을 병합함. 최종적으로 하나의 run이 남을 때까지 반복. */
	do {
		struct list_elem *a0;     /* 첫 번째 run의 시작. */
		struct list_elem *a1b0;   /* 첫 번째 run의 끝, 두 번째 run의 시작. */
		struct list_elem *b1;     /* 두 번째 run의 끝. */

		output_run_cnt = 0;
		for (a0 = list_begin (list); a0 != list_end (list); a0 = b1) {
			/* 각 반복에서 하나의 출력 run이 생성됨. */
			output_run_cnt++;

			/* 비내림차순 요소의 인접한 두 run을 찾음. */
			a1b0 = find_end_of_run (a0, list_end (list), less, aux);
			if (a1b0 == list_end (list))
				break;
			b1 = find_end_of_run (a1b0, list_end (list), less, aux);

			/* 두 run을 병합. */
			inplace_merge (a0, a1b0, b1, less, aux);
		}
	}
	while (output_run_cnt > 1);

	ASSERT (is_sorted (list_begin (list), list_end (list), less, aux));
}

/* LIST에 ELEM을 올바른 위치에 삽입함. 
   LIST는 LESS와 AUX에 따라 정렬되어 있어야 함.
   LIST의 요소 수에 대해 평균적으로 O(n) 시간이 소요됨. */
void
list_insert_ordered (struct list *list, struct list_elem *elem, list_less_func *less, void *aux) {
	struct list_elem *e;

	ASSERT (list != NULL);
	ASSERT (elem != NULL);
	ASSERT (less != NULL);

	for (e = list_begin (list); e != list_end (list); e = list_next (e))
		if (less (elem, e, aux))
			break;
	return list_insert (e, elem);
}

/* LIST를 순회하며, LESS와 AUX에 따라 인접한 요소 중 동일한 요소를 제거함.
   DUPLICATES가 NULL이 아닌 경우 LIST에서 제거된 요소를 DUPLICATES에 추가함. */
void
list_unique (struct list *list, struct list *duplicates,
		list_less_func *less, void *aux) {
	struct list_elem *elem, *next;

	ASSERT (list != NULL);
	ASSERT (less != NULL);
	if (list_empty (list))
		return;

	elem = list_begin (list);
	while ((next = list_next (elem)) != list_end (list))
		if (!less (elem, next, aux) && !less (next, elem, aux)) {
			list_remove (next);
			if (duplicates != NULL)
				list_push_back (duplicates, next);
		} else
			elem = next;
}

/* LIST에서 LESS와 AUX에 따라 가장 큰 값을 가진 요소를 반환. 
   최대값이 여러 개일 경우 리스트에서 먼저 나타난 요소를 반환. 
   리스트가 비어 있으면 tail을 반환. */
struct list_elem *
list_max (struct list *list, list_less_func *less, void *aux) {
	struct list_elem *max = list_begin (list);
	if (max != list_end (list)) {
		struct list_elem *e;

		for (e = list_next (max); e != list_end (list); e = list_next (e))
			if (less (max, e, aux))
				max = e;
	}
	return max;
}

/* LIST에서 LESS와 AUX에 따라 가장 작은 값을 가진 요소를 반환. 
   최소값이 여러 개일 경우 리스트에서 먼저 나타난 요소를 반환. 
   리스트가 비어 있으면 tail을 반환. */
struct list_elem *
list_min (struct list *list, list_less_func *less, void *aux) {
	struct list_elem *min = list_begin (list);
	if (min != list_end (list)) {
		struct list_elem *e;

		for (e = list_next (min); e != list_end (list); e = list_next (e))
			if (less (e, min, aux))
				min = e;
	}
	return min;
}
