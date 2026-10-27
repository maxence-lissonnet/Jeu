// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBBossAIController.h"

#include "Emberfall.h"
#include "EMBBossBase.h"
#include "EMBDragonTypes.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"

const FName AEMBBossAIController::BBKey_TargetActor(TEXT("TargetActor"));
const FName AEMBBossAIController::BBKey_TargetDistance(TEXT("TargetDistance"));
const FName AEMBBossAIController::BBKey_TargetAngle(TEXT("TargetAngle"));
const FName AEMBBossAIController::BBKey_CurrentPhase(TEXT("CurrentPhase"));
const FName AEMBBossAIController::BBKey_IsFlying(TEXT("bIsFlying"));
const FName AEMBBossAIController::BBKey_IsStunned(TEXT("bIsStunned"));
const FName AEMBBossAIController::BBKey_IsTransitioning(TEXT("bIsTransitioning"));
const FName AEMBBossAIController::BBKey_IsDead(TEXT("bIsDead"));
const FName AEMBBossAIController::BBKey_IsVulnerable(TEXT("bIsVulnerable"));
const FName AEMBBossAIController::BBKey_CataclysmDue(TEXT("bCataclysmDue"));
const FName AEMBBossAIController::BBKey_ThreatCount(TEXT("ThreatCount"));
const FName AEMBBossAIController::BBKey_SelectedAttackTag(TEXT("SelectedAttackTag"));
const FName AEMBBossAIController::BBKey_SelectedAttackKind(TEXT("SelectedAttackKind"));
const FName AEMBBossAIController::BBKey_SelectedAttackDuration(TEXT("SelectedAttackDuration"));

AEMBBossAIController::AEMBBossAIController()
{
	// Le Tick du contrôleur DOIT rester actif : AAIController::Tick appelle UpdateControlRotation, qui applique le
	// focus (SetFocus) à la rotation du Pawn. Le désactiver figerait l'orientation du boss. Le Tick d'un
	// AIController est très léger (une rotation) ; la logique de combat, elle, reste pilotée par le BT.
	PrimaryActorTick.bCanEverTick = true;

	UAIPerceptionComponent* Perception = CreateDefaultSubobject<UAIPerceptionComponent>(TEXT("AIPerception"));
	SetPerceptionComponent(*Perception);

	// Vue sur 80 m, champ de 360 degrés : le boss n'a pas d'angle mort (la lecture de coups repose sur ses
	// télégraphes, pas sur une perception limitée). Les joueurs n'ont pas d'équipe : ils sont "neutres" pour
	// la perception, d'où les trois affiliations activées. Le projet doit conserver l'option par défaut
	// "Auto register all pawns as sources" (Project Settings > AI > AI Perception), sinon ajouter un
	// UAIPerceptionStimuliSourceComponent au personnage joueur.
	SightConfig = CreateDefaultSubobject<UAISenseConfig_Sight>(TEXT("SightConfig"));
	SightConfig->SightRadius = EMBDragonDefaults::PerceptionRadius;
	SightConfig->LoseSightRadius = EMBDragonDefaults::PerceptionRadius + 1000.f;
	SightConfig->PeripheralVisionAngleDegrees = 180.f;
	SightConfig->SetMaxAge(5.f);
	SightConfig->DetectionByAffiliation.bDetectEnemies = true;
	SightConfig->DetectionByAffiliation.bDetectNeutrals = true;
	SightConfig->DetectionByAffiliation.bDetectFriendlies = true;

	Perception->ConfigureSense(*SightConfig);
	Perception->SetDominantSense(SightConfig->GetSenseImplementation());
}

void AEMBBossAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	if (UAIPerceptionComponent* Perception = GetAIPerceptionComponent())
	{
		Perception->OnTargetPerceptionUpdated.AddUniqueDynamic(this, &AEMBBossAIController::HandleTargetPerceptionUpdated);
	}

	if (BlackboardAsset)
	{
		UBlackboardComponent* BlackboardComp = nullptr;
		UseBlackboard(BlackboardAsset, BlackboardComp);
	}

	const AEMBBossBase* PossessedBoss = Cast<AEMBBossBase>(InPawn);
	if (PossessedBoss && PossessedBoss->IsDead())
	{
		// Boss déjà vaincu (sauvegarde) ou mort avant la possession : l'IA ne démarre pas.
	}
	else if (BehaviorTreeAsset)
	{
		RunBehaviorTree(BehaviorTreeAsset);
	}
	else
	{
		UE_LOG(LogEmberfall, Warning, TEXT("AEMBBossAIController %s : BehaviorTreeAsset non assigne, le boss restera inactif."), *GetName());
	}

	SyncBlackboardState();
}

void AEMBBossAIController::OnUnPossess()
{
	if (UAIPerceptionComponent* Perception = GetAIPerceptionComponent())
	{
		Perception->OnTargetPerceptionUpdated.RemoveDynamic(this, &AEMBBossAIController::HandleTargetPerceptionUpdated);
	}
	CombatTarget.Reset();

	Super::OnUnPossess();
}

void AEMBBossAIController::HandleTargetPerceptionUpdated(AActor* Actor, FAIStimulus Stimulus)
{
	if (!Actor || !Stimulus.WasSuccessfullySensed())
	{
		return;
	}

	// Seuls les Pawns contrôlés par un joueur déclenchent le combat (pas un PNJ ni un familier).
	const APawn* SensedPawn = Cast<APawn>(Actor);
	if (!SensedPawn || !SensedPawn->IsPlayerControlled())
	{
		return;
	}

	if (AEMBBossBase* Boss = Cast<AEMBBossBase>(GetPawn()))
	{
		// Idempotent : la mise à l'échelle coop n'a lieu qu'une fois. Seul un joueur dans l'arène (ArenaRadius) déclenche
		// la rencontre : une détection lointaine (vue sur 80 m) ne doit pas figer l'échelle trop tôt.
		if (Boss->CountPlayersWithin(Boss->GetArenaRadius()) > 0)
		{
			Boss->BeginEncounter();
		}
	}

	if (!CombatTarget.IsValid())
	{
		SetCombatTarget(Actor);
	}
}

void AEMBBossAIController::SyncBlackboardState()
{
	UBlackboardComponent* BlackboardComp = GetBlackboardComponent();
	const AEMBBossBase* Boss = Cast<AEMBBossBase>(GetPawn());
	if (!BlackboardComp || !Boss)
	{
		return;
	}

	BlackboardComp->SetValueAsInt(BBKey_CurrentPhase, Boss->GetCurrentPhase());
	BlackboardComp->SetValueAsBool(BBKey_IsFlying, Boss->IsFlying());
	BlackboardComp->SetValueAsBool(BBKey_IsStunned, Boss->IsStunned());
	BlackboardComp->SetValueAsBool(BBKey_IsTransitioning, Boss->IsTransitioning());
	BlackboardComp->SetValueAsBool(BBKey_IsDead, Boss->IsDead());
	BlackboardComp->SetValueAsBool(BBKey_IsVulnerable, Boss->IsVulnerable());
	BlackboardComp->SetValueAsBool(BBKey_CataclysmDue, Boss->IsScriptedAttackDue());
}

void AEMBBossAIController::SetCombatTarget(AActor* NewTarget)
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

void AEMBBossAIController::SetFacingLocked(const bool bLocked)
{
	bFacingLocked = bLocked;
	ApplyFocus();
}

void AEMBBossAIController::ApplyFocus()
{
	// Le focus fait pivoter le Pawn vers la cible (CharacterMovement : bUseControllerDesiredRotation).
	// Pendant l'engagement d'une attaque, le focus est retiré : le boss garde son cap, ce qui permet
	// au joueur de lire la trajectoire et de l'esquiver.
	if (bFacingLocked || !CombatTarget.IsValid())
	{
		ClearFocus(EAIFocusPriority::Gameplay);
	}
	else
	{
		SetFocus(CombatTarget.Get(), EAIFocusPriority::Gameplay);
	}
}
