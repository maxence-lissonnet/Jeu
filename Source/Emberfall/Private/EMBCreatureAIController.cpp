// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBCreatureAIController.h"

#include "Emberfall.h"
#include "EMBBossBase.h"
#include "EMBCreatureBase.h"
#include "EMBCreatureTypes.h"
#include "EMBEnvironmentSubsystem.h"
#include "EMBWeatherSubsystem.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "NavigationSystem.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISense_Hearing.h"
#include "Perception/AISense_Sight.h"
#include "Perception/AISenseConfig_Hearing.h"
#include "Perception/AISenseConfig_Sight.h"

const FName AEMBCreatureAIController::BBKey_TargetActor(TEXT("TargetActor"));
const FName AEMBCreatureAIController::BBKey_TargetDistance(TEXT("TargetDistance"));
const FName AEMBCreatureAIController::BBKey_HomeLocation(TEXT("HomeLocation"));
const FName AEMBCreatureAIController::BBKey_PatrolLocation(TEXT("PatrolLocation"));
const FName AEMBCreatureAIController::BBKey_IsDead(TEXT("bIsDead"));
const FName AEMBCreatureAIController::BBKey_IsStaggered(TEXT("bIsStaggered"));
const FName AEMBCreatureAIController::BBKey_ShouldFlee(TEXT("bShouldFlee"));
const FName AEMBCreatureAIController::BBKey_ReturnHome(TEXT("bReturnHome"));
const FName AEMBCreatureAIController::BBKey_TargetInAttackRange(TEXT("bTargetInAttackRange"));

namespace
{
	/** Durée (s) pendant laquelle un stimulus reste valable après la perte du contact : mémoire de la perception. */
	constexpr float SightMemorySeconds = 3.f;
	constexpr float HearingMemorySeconds = 2.f;

	/** Champ de vision : demi-angle de 100 degrés (200 au total), bien plus large qu'un humain. */
	constexpr float PeripheralVisionHalfAngleDeg = 100.f;

	/** Marge entre le rayon de vue et le rayon de perte de vue configurés dans le composant. */
	constexpr float LoseSightMargin = 400.f;

	/** Bonus de score de la cible actuelle (évite d'osciller entre deux joueurs à distance égale). */
	constexpr float CreatureCurrentTargetBonus = 1.25f;
}

AEMBCreatureAIController::AEMBCreatureAIController()
{
	// Le Tick du contrôleur DOIT rester actif : AAIController::Tick applique le focus (SetFocus) à la rotation du pawn.
	// Il est très léger ; la logique de combat reste pilotée par le BT. Le palier "réduit" de la significance
	// l'espace à 0,25 s (SetActorTickInterval).
	PrimaryActorTick.bCanEverTick = true;

	UAIPerceptionComponent* Perception = CreateDefaultSubobject<UAIPerceptionComponent>(TEXT("AIPerception"));
	SetPerceptionComponent(*Perception);

	// Les joueurs n'ont pas d'équipe : ils sont "neutres" pour la perception, d'où les trois affiliations activées.
	// Le projet doit conserver l'option par défaut "Auto register all pawns as sources" (Project Settings > AI >
	// AI Perception), sinon ajouter un UAIPerceptionStimuliSourceComponent au personnage joueur.
	SightConfig = CreateDefaultSubobject<UAISenseConfig_Sight>(TEXT("SightConfig"));
	SightConfig->SightRadius = EMBCreatureDefaults::SightRadius;
	SightConfig->LoseSightRadius = EMBCreatureDefaults::SightRadius + LoseSightMargin;
	SightConfig->PeripheralVisionAngleDegrees = PeripheralVisionHalfAngleDeg;
	SightConfig->SetMaxAge(SightMemorySeconds);
	SightConfig->DetectionByAffiliation.bDetectEnemies = true;
	SightConfig->DetectionByAffiliation.bDetectNeutrals = true;
	SightConfig->DetectionByAffiliation.bDetectFriendlies = true;

	HearingConfig = CreateDefaultSubobject<UAISenseConfig_Hearing>(TEXT("HearingConfig"));
	HearingConfig->HearingRange = EMBCreatureDefaults::HearingRadius;
	HearingConfig->SetMaxAge(HearingMemorySeconds);
	HearingConfig->DetectionByAffiliation.bDetectEnemies = true;
	HearingConfig->DetectionByAffiliation.bDetectNeutrals = true;
	HearingConfig->DetectionByAffiliation.bDetectFriendlies = true;

	Perception->ConfigureSense(*SightConfig);
	Perception->ConfigureSense(*HearingConfig);
	Perception->SetDominantSense(SightConfig->GetSenseImplementation());
}

void AEMBCreatureAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	ConfigurePerceptionFromRow();

	if (BlackboardAsset)
	{
		UBlackboardComponent* BlackboardComp = nullptr;
		UseBlackboard(BlackboardAsset, BlackboardComp);
	}

	const AEMBCreatureBase* Creature = Cast<AEMBCreatureBase>(InPawn);
	if (Creature && Creature->IsDead())
	{
		// Mort avant la possession : l'IA ne démarre pas.
	}
	else if (BehaviorTreeAsset)
	{
		RunBehaviorTree(BehaviorTreeAsset);
	}
	else
	{
		UE_LOG(LogEmberfall, Warning, TEXT("AEMBCreatureAIController %s : BehaviorTreeAsset non assigne, la creature restera inactive."), *GetName());
	}

	if (UBlackboardComponent* BlackboardComp = GetBlackboardComponent())
	{
		if (Creature)
		{
			BlackboardComp->SetValueAsVector(BBKey_HomeLocation, Creature->GetHomeLocation());
		}
	}
	SyncBlackboardState();
}

void AEMBCreatureAIController::OnUnPossess()
{
	CombatTarget.Reset();
	Super::OnUnPossess();
}

void AEMBCreatureAIController::ConfigurePerceptionFromRow()
{
	// Les portées de la ligne remplacent les valeurs par défaut du constructeur (la vue est ensuite pondérée par la
	// visibilité météo dans FindBestPlayerTarget : la portée configurée est le maximum par temps clair).
	const AEMBCreatureBase* Creature = Cast<AEMBCreatureBase>(GetPawn());
	UAIPerceptionComponent* Perception = GetAIPerceptionComponent();
	if (!Creature || !Creature->HasCreatureRow() || !Perception)
	{
		return;
	}

	const FEMBCreatureRow& Row = Creature->GetCreatureRow();
	SightConfig->SightRadius = Row.SightRadius;
	SightConfig->LoseSightRadius = Row.SightRadius * EMBCreatureDefaults::LoseSightFactor + LoseSightMargin;
	HearingConfig->HearingRange = Row.HearingRadius;

	// ConfigureSense remplace la configuration enregistrée et demande la mise à jour des écouteurs.
	Perception->ConfigureSense(*SightConfig);
	Perception->ConfigureSense(*HearingConfig);
}

void AEMBCreatureAIController::SyncBlackboardState()
{
	UBlackboardComponent* BlackboardComp = GetBlackboardComponent();
	const AEMBCreatureBase* Creature = Cast<AEMBCreatureBase>(GetPawn());
	if (!BlackboardComp || !Creature)
	{
		return;
	}

	BlackboardComp->SetValueAsBool(BBKey_IsDead, Creature->IsDead());
	BlackboardComp->SetValueAsBool(BBKey_IsStaggered, Creature->IsStaggered());
}

void AEMBCreatureAIController::SetCombatTarget(AActor* NewTarget)
{
	CombatTarget = NewTarget;

	if (UBlackboardComponent* BlackboardComp = GetBlackboardComponent())
	{
		if (NewTarget)
		{
			BlackboardComp->SetValueAsObject(BBKey_TargetActor, NewTarget);
		}
		else
		{
			BlackboardComp->ClearValue(BBKey_TargetActor);
		}
	}

	ApplyFocus();
}

void AEMBCreatureAIController::SetFacingLocked(const bool bLocked)
{
	bFacingLocked = bLocked;
	ApplyFocus();
}

void AEMBCreatureAIController::ApplyFocus()
{
	// Le focus fait pivoter le pawn vers la cible (CharacterMovement : bUseControllerDesiredRotation). Pendant
	// l'engagement d'une attaque il est retiré : la créature garde son cap, ce qui rend le coup esquivable.
	if (bFacingLocked || !CombatTarget.IsValid())
	{
		ClearFocus(EAIFocusPriority::Gameplay);
	}
	else
	{
		SetFocus(CombatTarget.Get(), EAIFocusPriority::Gameplay);
	}
}

AActor* AEMBCreatureAIController::FindBestPlayerTarget(const AActor* CurrentTarget)
{
	const AEMBCreatureBase* Creature = Cast<AEMBCreatureBase>(GetPawn());
	UAIPerceptionComponent* Perception = GetAIPerceptionComponent();
	const UWorld* World = GetWorld();
	if (!Creature || !Creature->HasCreatureRow() || !Perception || !World)
	{
		return nullptr;
	}

	const FEMBCreatureRow& Row = Creature->GetCreatureRow();
	const FVector Origin = Creature->GetActorLocation();

	// Vue : portée de la ligne x visibilité météo (brouillard x0,6, tempête x0,7...) à l'endroit de la créature.
	float Visibility = 1.f;
	if (const UEMBWeatherSubsystem* Weather = World->GetSubsystem<UEMBWeatherSubsystem>())
	{
		Visibility = Weather->GetVisibilityMultiplierAt(Origin);
	}
	const float EffectiveSightRadius = Row.SightRadius * Visibility;

	// Sources perçues par le composant (le stimulus reste valable SightMemorySeconds après la perte de contact).
	TArray<AActor*> Seen;
	TArray<AActor*> Heard;
	Perception->GetCurrentlyPerceivedActors(UAISense_Sight::StaticClass(), Seen);
	Perception->GetCurrentlyPerceivedActors(UAISense_Hearing::StaticClass(), Heard);

	const UEMBEnvironmentSubsystem* Environment = World->GetSubsystem<UEMBEnvironmentSubsystem>();
	const APawn* RecentAttacker = Creature->GetRecentAttacker(EMBCreatureDefaults::AggroMemory);
	const float ReferenceRange = FMath::Max(FMath::Max(Row.SightRadius, Row.HearingRadius), 1.f);

	TArray<APawn*> Players;
	AEMBBossBase::CollectAlivePlayerPawns(World, Players);

	AActor* Best = nullptr;
	float BestScore = -1.f;
	for (APawn* Player : Players)
	{
		const float Distance = static_cast<float>(FVector::Dist2D(Origin, Player->GetActorLocation()));

		// Une créature qui craint le feu ne poursuit pas un joueur qui se tient dans la lumière d'un feu.
		if (Row.bFleesFire && Environment
			&& Environment->GetHeatBonusAt(Player->GetActorLocation()) >= EMBCreatureDefaults::FleeHeatBonusThreshold)
		{
			continue;
		}

		const bool bIsCurrent = Player == CurrentTarget;
		const float SightLimit = bIsCurrent ? EffectiveSightRadius * EMBCreatureDefaults::LoseSightFactor : EffectiveSightRadius;

		bool bDetected = false;
		if (Distance <= SightLimit && Seen.Contains(Player))
		{
			bDetected = true;
		}
		else if (Distance <= Row.HearingRadius
			&& (Heard.Contains(Player) || Player->GetVelocity().SizeSquared2D() >= FMath::Square(EMBCreatureDefaults::HearingMinSpeed)))
		{
			// Ouïe : bruit signalé au système de perception, ou pas de course/sprint (un joueur immobile ou discret
			// n'est pas entendu).
			bDetected = true;
		}
		else if (Player == RecentAttacker && Distance <= Row.LeashRadius)
		{
			bDetected = true; // celui qui vient de frapper est toujours une cible
		}

		if (!bDetected)
		{
			continue;
		}

		float Score = 1.f - FMath::Clamp(Distance / ReferenceRange, 0.f, 1.f);
		if (bIsCurrent)
		{
			Score *= CreatureCurrentTargetBonus;
		}
		// Un score nul (joueur au bord de la portée) reste une cible valide : plancher strictement positif.
		Score = FMath::Max(Score, KINDA_SMALL_NUMBER);
		if (Score > BestScore)
		{
			BestScore = Score;
			Best = Player;
		}
	}
	return Best;
}

bool AEMBCreatureAIController::PickPatrolLocation()
{
	const AEMBCreatureBase* Creature = Cast<AEMBCreatureBase>(GetPawn());
	UBlackboardComponent* BlackboardComp = GetBlackboardComponent();
	UNavigationSystemV1* NavSystem = UNavigationSystemV1::GetCurrent(GetWorld());
	if (!Creature || !Creature->HasCreatureRow() || !BlackboardComp || !NavSystem)
	{
		return false;
	}

	FNavLocation Result;
	if (!NavSystem->GetRandomReachablePointInRadius(Creature->GetHomeLocation(), Creature->GetCreatureRow().PatrolRadius, Result))
	{
		return false;
	}

	BlackboardComp->SetValueAsVector(BBKey_PatrolLocation, Result.Location);
	LastPatrolPickTime = GetWorld()->GetTimeSeconds();
	return true;
}
